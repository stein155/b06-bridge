#pragma once

// Framework-neutral protocol code of the B06 display (BIKEGO plus app) over
// the Microchip transparent UART service. Frames in both directions:
//   0xA5 [len] [cmd] [payload...] [crc_lo] [crc_hi] 0x0D
// len counts the payload and the checksum is CRC-16/XMODEM over len..payload.
// The display needs an AES challenge-response and then pushes its ride and
// battery frames on its own; it is never polled.

#include <cstddef>
#include <cstdint>

namespace esphome::b06::protocol {

static constexpr uint8_t FRAME_START = 0xA5;
static constexpr uint8_t FRAME_END = 0x0D;
static constexpr size_t OVERHEAD = 6;  // start, len, cmd, crc, crc, end

static constexpr uint8_t CMD_AUTH_START = 0x20;
static constexpr uint8_t CMD_AUTH_ANSWER = 0x21;
static constexpr uint8_t CMD_VERSION = 0x40;
static constexpr uint8_t CMD_RIDE = 0x41;
static constexpr uint8_t CMD_BATTERY = 0x42;
static constexpr uint8_t CMD_CONFIG = 0x43;
static constexpr size_t AUTH_CHALLENGE_LEN = 4;

struct BikeState {
    int battery = -1;
    int trip_duration = -1;
    float speed_kmh = -1;
    float max_speed_kmh = -1;
    float avg_speed_kmh = -1;
    float trip_distance_km = -1;
    float odometer_km = -1;
    bool light_on = false;
    bool light_valid = false;
    char firmware[16] = {0};
};

uint16_t crc16(const uint8_t *data, size_t len);

// Builds a frame; out must hold len + OVERHEAD bytes.
size_t build_frame(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len);
// Builds the configuration write that switches the light; out must hold
// 4 + OVERHEAD bytes.
size_t build_light(uint8_t *out, bool on);

// The challenge-response: AES-128-ECB with a key fixed in the BIKEGO plus app
// over the 4-byte challenge padded with zeros to one block.
bool auth_answer(const uint8_t challenge[AUTH_CHALLENGE_LEN], uint8_t answer[16]);

// Reassembles notification chunks into frames, decodes the ride, battery and
// version frames into the BikeState and keeps the authentication replies for
// the component.
class FrameParser {
 public:
    static constexpr size_t BUFFER_SIZE = 2 * (0xFF + OVERHEAD);

    void reset() {
        this->len_ = 0;
        this->frames_ = 0;
        this->challenge_ready_ = false;
        this->auth_result_ = AuthResult::NONE;
    }
    // Appends data and decodes every complete frame. Returns true if the state
    // changed; frames failing the CRC check are counted in bad_crc, valid
    // frames without anything we decode in unhandled.
    bool feed(BikeState &state, const uint8_t *data, size_t len, uint32_t &bad_crc, uint32_t &unhandled);

    // The display's challenge, once per received challenge frame.
    bool take_challenge(uint8_t out[AUTH_CHALLENGE_LEN]);
    enum class AuthResult : uint8_t { NONE, OK, FAILED };
    AuthResult auth_result() const { return this->auth_result_; }
    uint32_t frames() const { return this->frames_; }

 private:
    // Returns false for frames we do not know; sets changed when state was updated.
    bool handle_frame_(BikeState &state, uint8_t cmd, const uint8_t *payload, uint8_t len, bool &changed);

    uint8_t buffer_[BUFFER_SIZE]{};
    size_t len_{0};
    uint32_t frames_{0};
    uint8_t challenge_[AUTH_CHALLENGE_LEN]{};
    bool challenge_ready_{false};
    AuthResult auth_result_{AuthResult::NONE};
};

}  // namespace esphome::b06::protocol
