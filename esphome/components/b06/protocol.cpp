#include "protocol.h"

#include <cstring>

#include "mbedtls/cipher.h"

namespace esphome::b06::protocol {

// Payload sizes the BIKEGO plus app 1.6.1 decodes.
static constexpr uint8_t RIDE_LEN = 29;
static constexpr uint8_t BATTERY_LEN = 6;

// CRC-16/XMODEM (polynomial 0x1021, init 0).
uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t) data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

size_t build_frame(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len) {
    out[0] = FRAME_START;
    out[1] = len;
    out[2] = cmd;
    if (len > 0)
        memcpy(out + 3, payload, len);
    const uint16_t crc = crc16(out + 1, 2 + len);
    out[3 + len] = crc & 0xFF;
    out[4 + len] = crc >> 8;
    out[5 + len] = FRAME_END;
    return OVERHEAD + len;
}

size_t build_light(uint8_t *out, bool on) {
    // [write] [CONTENT_CONFIG_LIGHT] [value], as the app's MeterConfig.lightOn/lightOff.
    const uint8_t payload[] = {0x02, 0x0E, 0x01, (uint8_t) (on ? 1 : 0)};
    return build_frame(out, CMD_CONFIG, payload, sizeof(payload));
}

static uint16_t u16_le(const uint8_t *data) {
    return data[0] | (data[1] << 8);
}
static uint32_t u32_le(const uint8_t *data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t) data[3] << 24);
}

bool FrameParser::take_challenge(uint8_t out[AUTH_CHALLENGE_LEN]) {
    if (!this->challenge_ready_)
        return false;
    memcpy(out, this->challenge_, AUTH_CHALLENGE_LEN);
    this->challenge_ready_ = false;
    return true;
}

bool FrameParser::handle_frame_(BikeState &state, uint8_t cmd, const uint8_t *payload, uint8_t len, bool &changed) {
    switch (cmd) {
        case CMD_AUTH_START:
            if (len < AUTH_CHALLENGE_LEN)
                return false;
            memcpy(this->challenge_, payload, AUTH_CHALLENGE_LEN);
            this->challenge_ready_ = true;
            return true;
        case CMD_AUTH_ANSWER:
            if (len == 0)
                return false;
            this->auth_result_ = payload[0] == 0 ? AuthResult::OK : AuthResult::FAILED;
            return true;
        case CMD_VERSION: {
            // [protocol type] then the software and hardware version as ASCII,
            // separated by a non-printable byte; keep the software version.
            size_t j = 0;
            for (uint8_t i = 1; i < len && j < sizeof(state.firmware) - 1; i++) {
                if (payload[i] < 32 || payload[i] > 126)
                    break;
                state.firmware[j++] = (char) payload[i];
            }
            // An empty reply keeps the version that is already known.
            if (j > 0) {
                state.firmware[j] = '\0';
                changed = true;
            }
            return true;
        }
        case CMD_RIDE:
            if (len < RIDE_LEN)
                return false;
            state.speed_kmh = u16_le(payload) / 10.0f;
            state.max_speed_kmh = u16_le(payload + 2) / 10.0f;
            state.avg_speed_kmh = u16_le(payload + 4) / 100.0f;
            // Bytes 6-7 are the app's "range", which it never displays; on a
            // real display they read 16665 at 81 % battery, so they are skipped.
            state.trip_duration = (int) u32_le(payload + 8);
            state.trip_distance_km = u32_le(payload + 12) / 100.0f;
            // The odometer, as on the King Meter bikes.
            state.odometer_km = u32_le(payload + 16) / 100.0f;
            state.light_on = payload[21] != 0;
            state.light_valid = true;
            changed = true;
            return true;
        case CMD_BATTERY:
            if (len < BATTERY_LEN)
                return false;
            state.battery = payload[0];
            changed = true;
            return true;
        default:
            return false;
    }
}

bool FrameParser::feed(BikeState &state, const uint8_t *data, size_t len, uint32_t &bad_crc, uint32_t &unhandled) {
    bool changed = false;
    if (data != nullptr && len > 0) {
        // Overflow means garbage, so start over.
        if (len > BUFFER_SIZE - this->len_)
            this->len_ = 0;
        if (len > BUFFER_SIZE)
            return false;
        memcpy(this->buffer_ + this->len_, data, len);
        this->len_ += len;
    }

    while (true) {
        size_t start = 0;
        while (start < this->len_ && this->buffer_[start] != FRAME_START)
            ++start;
        if (start > 0) {
            memmove(this->buffer_, this->buffer_ + start, this->len_ - start);
            this->len_ -= start;
        }
        if (this->len_ < 2)
            break;
        const size_t frame_len = this->buffer_[1] + OVERHEAD;
        if (this->len_ < frame_len)
            break;
        const uint8_t *frame = this->buffer_;
        const uint8_t payload_len = frame[1];
        if (frame[frame_len - 1] != FRAME_END) {
            // Not a frame start after all, search again.
            memmove(this->buffer_, this->buffer_ + 1, this->len_ - 1);
            --this->len_;
            continue;
        }

        const uint16_t crc = crc16(frame + 1, 2 + payload_len);
        if (frame[3 + payload_len] != (crc & 0xFF) || frame[4 + payload_len] != (crc >> 8)) {
            ++bad_crc;
        } else {
            ++this->frames_;
            if (!this->handle_frame_(state, frame[2], frame + 3, payload_len, changed))
                ++unhandled;
        }

        memmove(this->buffer_, this->buffer_ + frame_len, this->len_ - frame_len);
        this->len_ -= frame_len;
    }
    return changed;
}

// KEY_AUTH_SECRET of the BIKEGO plus app 1.6.1.
static const uint8_t AUTH_KEY[16] = {0x94, 0x93, 0x4B, 0xB3, 0x01, 0xB0, 0xE4, 0x0F,
                                     0x2E, 0xC0, 0x5B, 0x04, 0x58, 0x8B, 0xAD, 0xD4};

bool auth_answer(const uint8_t challenge[AUTH_CHALLENGE_LEN], uint8_t answer[16]) {
    // The generic cipher API also works where ESP-IDF swaps in its hardware
    // AES driver.
    const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (info == nullptr)
        return false;
    uint8_t block[16] = {0};
    memcpy(block, challenge, AUTH_CHALLENGE_LEN);
    mbedtls_cipher_context_t ctx;
    mbedtls_cipher_init(&ctx);
    size_t out_len = 0;
    const bool ok =
        mbedtls_cipher_setup(&ctx, info) == 0 && mbedtls_cipher_setkey(&ctx, AUTH_KEY, 128, MBEDTLS_ENCRYPT) == 0 &&
        mbedtls_cipher_update(&ctx, block, sizeof(block), answer, &out_len) == 0 && out_len == sizeof(block);
    mbedtls_cipher_free(&ctx);
    return ok;
}

}  // namespace esphome::b06::protocol
