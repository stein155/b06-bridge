// Host tests of the framework-neutral protocol code. Expected bytes come from
// the reference implementation in Python (b06-reader), written from the
// BIKEGO plus app 1.6.1.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

using namespace esphome::b06::protocol;
using Bytes = std::vector<uint8_t>;

static int failures = 0;

static std::string hex(const uint8_t *data, size_t len) {
    std::string out;
    char buf[4];
    for (size_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", data[i]);
        out += buf;
    }
    return out;
}

static void check(bool ok, const char *what) {
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

static void check_bytes(const uint8_t *data, size_t len, const Bytes &expected, const char *what) {
    const bool ok = len == expected.size() && memcmp(data, expected.data(), len) == 0;
    if (!ok)
        printf("     got      %s\n     expected %s\n", hex(data, len).c_str(),
               hex(expected.data(), expected.size()).c_str());
    check(ok, what);
}

static void test_b06() {
    uint8_t frame[64];
    static const uint8_t START[] = {0x01};
    check_bytes(frame, build_frame(frame, CMD_AUTH_START, START, 1), {0xA5, 0x01, 0x20, 0x01, 0xF7, 0x21, 0x0D},
                "B06 authentication request");
    check_bytes(frame, build_frame(frame, CMD_VERSION, nullptr, 0), {0xA5, 0x00, 0x40, 0xC4, 0x48, 0x0D},
                "B06 version request");

    static const uint8_t CHALLENGE[] = {0x11, 0x22, 0x33, 0x44};
    uint8_t answer[16];
    check(auth_answer(CHALLENGE, answer), "B06 authentication answer computed");
    check_bytes(answer, sizeof(answer),
                {0xDE, 0x5C, 0x8F, 0x89, 0xEC, 0x8D, 0xDE, 0xEC, 0x82, 0x1F, 0xAB, 0xE8, 0xCE, 0xF2, 0xB3, 0x19},
                "B06 authentication answer");

    // Same as the app's MeterConfig.lightOn: 0x43 [write, CONTENT_CONFIG_LIGHT, on].
    const size_t light_len = build_light(frame, true);
    check(light_len == 10 && frame[2] == 0x43 && frame[3] == 0x02 && frame[4] == 0x0E && frame[5] == 0x01 &&
              frame[6] == 0x01,
          "B06 light on");
    const uint16_t light_crc = crc16(frame + 1, 6);
    check(frame[7] == (light_crc & 0xFF) && frame[8] == (light_crc >> 8) && frame[9] == 0x0D, "B06 light frame");

    FrameParser parser;
    BikeState state;
    uint32_t bad = 0, unhandled = 0;

    // Challenge and accepted answer: handled, but no telemetry.
    Bytes auth(frame, frame + build_frame(frame, CMD_AUTH_START, CHALLENGE, 4));
    check(!parser.feed(state, auth.data(), auth.size(), bad, unhandled), "B06 challenge is not telemetry");
    uint8_t challenge[4];
    check(parser.take_challenge(challenge) && memcmp(challenge, CHALLENGE, 4) == 0, "B06 challenge kept");
    check(!parser.take_challenge(challenge), "B06 challenge taken once");
    static const uint8_t ACCEPTED[] = {0x00};
    Bytes ok(frame, frame + build_frame(frame, CMD_AUTH_ANSWER, ACCEPTED, 1));
    parser.feed(state, ok.data(), ok.size(), bad, unhandled);
    check(parser.auth_result() == FrameParser::AuthResult::OK, "B06 authentication accepted");

    // Battery 80 %, split over two notifications.
    const Bytes battery = {0xA5, 0x06, 0x42, 0x50, 0x01, 0x9A, 0x00, 0x10, 0x00, 0x46, 0x4A, 0x0D};
    check(!parser.feed(state, battery.data(), 4, bad, unhandled), "B06 partial frame waits");
    check(parser.feed(state, battery.data() + 4, battery.size() - 4, bad, unhandled) && state.battery == 80,
          "B06 battery");

    // 30.0 km/h, max 45.5, average 18.25, range 50 km, 600 s, trip 12.34 km,
    // odometer 1000.00 km, light on.
    const uint8_t ride[29] = {0x2C, 0x01, 0xC7, 0x01, 0x21, 0x07, 0x00, 0x32, 0x58, 0x02, 0x00, 0x00, 0xD2, 0x04, 0x00,
                              0x00, 0xA0, 0x86, 0x01, 0x00, 3,    1,    0,    0,    0,    0,    0,    0,    0};
    Bytes ride_frame(frame, frame + build_frame(frame, CMD_RIDE, ride, sizeof(ride)));
    check(parser.feed(state, ride_frame.data(), ride_frame.size(), bad, unhandled), "B06 ride parsed");
    check(state.speed_kmh == 30.0f && state.max_speed_kmh == 45.5f && state.avg_speed_kmh == 18.25f, "B06 speeds");
    check(state.trip_duration == 600, "B06 ride time");
    check(state.trip_distance_km == 12.34f && state.odometer_km == 1000.0f, "B06 trip and odometer");
    check(state.light_valid && state.light_on, "B06 light");

    static const uint8_t VERSION[] = {0x01, 'B', '0', '6', '_', 'V', '1', 0x00, 'H', 'W', '1'};
    Bytes version(frame, frame + build_frame(frame, CMD_VERSION, VERSION, sizeof(VERSION)));
    parser.feed(state, version.data(), version.size(), bad, unhandled);
    check(strcmp(state.firmware, "B06_V1") == 0, "B06 software version");

    ride_frame[5] ^= 1;
    parser.feed(state, ride_frame.data(), ride_frame.size(), bad, unhandled);
    check(bad == 1 && unhandled == 0, "B06 bad CRC counted");
}

// Garbage, a false start byte and chunk boundaries must not lose real frames.
static void test_b06_parser_resync() {
    uint8_t frame[64];
    FrameParser parser;
    BikeState state;
    uint32_t bad = 0, unhandled = 0;

    static const uint8_t BATTERY_77[] = {77, 0, 0, 0, 0, 0};
    const size_t battery_len = build_frame(frame, CMD_BATTERY, BATTERY_77, sizeof(BATTERY_77));
    Bytes stream = {0x00, 0x13, 0xA5, 0x30, 0x41};  // noise and a start byte with an impossible frame
    stream.insert(stream.end(), frame, frame + battery_len);
    check(!parser.feed(state, stream.data(), stream.size() - battery_len + 2, bad, unhandled),
          "B06 waits while a false start byte claims more data");
    // The false frame's end byte is wrong once enough bytes arrived; the parser
    // then searches again from the next byte and finds the real frame.
    Bytes rest(stream.begin() + (stream.size() - battery_len + 2), stream.end());
    rest.resize(rest.size() + 48, 0x00);
    parser.feed(state, rest.data(), rest.size(), bad, unhandled);
    check(state.battery == 77, "B06 frame after a false start byte");

    // Two frames in one notification.
    static const uint8_t BATTERY_60[] = {60, 0, 0, 0, 0, 0};
    static const uint8_t BATTERY_61[] = {61, 0, 0, 0, 0, 0};
    Bytes two(frame, frame + build_frame(frame, CMD_BATTERY, BATTERY_60, sizeof(BATTERY_60)));
    const size_t second = build_frame(frame, CMD_BATTERY, BATTERY_61, sizeof(BATTERY_61));
    two.insert(two.end(), frame, frame + second);
    check(parser.feed(state, two.data(), two.size(), bad, unhandled) && state.battery == 61,
          "B06 two frames in one notification");

    // Too short for its command: counted as unhandled, state untouched.
    static const uint8_t SHORT_RIDE[] = {1, 2, 3};
    const uint32_t unhandled_before = unhandled;
    Bytes short_ride(frame, frame + build_frame(frame, CMD_RIDE, SHORT_RIDE, sizeof(SHORT_RIDE)));
    check(
        !parser.feed(state, short_ride.data(), short_ride.size(), bad, unhandled) && unhandled == unhandled_before + 1,
        "B06 short ride frame is unhandled");

    // A rejected authentication.
    static const uint8_t REJECTED[] = {0x01};
    Bytes rejected(frame, frame + build_frame(frame, CMD_AUTH_ANSWER, REJECTED, 1));
    parser.feed(state, rejected.data(), rejected.size(), bad, unhandled);
    check(parser.auth_result() == FrameParser::AuthResult::FAILED, "B06 authentication rejected");
}

// The version keeps its last value when a reply carries none, and is cut to
// fit the state.
static void test_b06_version() {
    uint8_t frame[64];
    FrameParser parser;
    BikeState state;
    uint32_t bad = 0, unhandled = 0;
    static const uint8_t VERSION[] = {0x01, 'V', '2'};
    Bytes version(frame, frame + build_frame(frame, CMD_VERSION, VERSION, sizeof(VERSION)));
    parser.feed(state, version.data(), version.size(), bad, unhandled);
    static const uint8_t EMPTY[] = {0x01};
    Bytes empty(frame, frame + build_frame(frame, CMD_VERSION, EMPTY, sizeof(EMPTY)));
    check(!parser.feed(state, empty.data(), empty.size(), bad, unhandled) && strcmp(state.firmware, "V2") == 0,
          "B06 empty version keeps the known one");
    uint8_t long_version[1 + 30];
    long_version[0] = 0x01;
    memset(long_version + 1, 'x', 30);
    Bytes longer(frame, frame + build_frame(frame, CMD_VERSION, long_version, sizeof(long_version)));
    parser.feed(state, longer.data(), longer.size(), bad, unhandled);
    check(strlen(state.firmware) == sizeof(state.firmware) - 1, "B06 long version is cut to fit");
}

int main() {
    test_b06();
    test_b06_parser_resync();
    test_b06_version();
    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All protocol tests passed\n");
    return 0;
}
