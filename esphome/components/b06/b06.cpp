#include "b06.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::b06 {

static const char *const TAG = "b06";

static constexpr uint32_t SUBSCRIBE_SETTLE_MS = 50;
static constexpr uint32_t CCCD_CONFIRM_TIMEOUT_MS = 1000;
// The BIKEGO plus app gives the display 10 s to answer the authentication.
static constexpr uint32_t AUTH_TIMEOUT_MS = 10000;
// Writes are spaced like the app's.
static constexpr uint32_t TX_SPACING_MS = 150;
// Reconnect when a connected display goes quiet.
static constexpr uint32_t DATA_TIMEOUT_MS = 15000;
static constexpr uint32_t FIRST_DATA_TIMEOUT_MS = 30000;
// The firmware version is asked for at most this often per connection.
static constexpr uint32_t VERSION_REQUEST_INTERVAL_MS = 15000;
static constexpr uint8_t VERSION_REQUEST_ATTEMPTS = 4;
// After max_connection_time the bike is left alone until it has not advertised
// for this long: it was switched off or ridden away.
static constexpr uint32_t BIKE_GONE_MS = 60000;
static constexpr uint32_t LIGHT_CONFIRM_WINDOW_MS = 2000;
// Log the first traffic of every connection at DEBUG, the rest at VERBOSE.
static constexpr uint32_t DEBUG_LOGGED = 20;
static constexpr uint32_t SUMMARY_INTERVAL_MS = 30000;
static constexpr size_t MAX_LOGGED_BYTES = 128;
// Bump when protocol::BikeState changes layout; older data is then ignored.
static constexpr uint32_t LAST_STATE_MAGIC = 0x42303601;
static constexpr uint32_t LAST_STATE_SAVE_INTERVAL_MS = 5 * 60 * 1000;

// Microchip transparent UART.
static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("49535343-fe7d-4ae5-8fa9-9fafd205e455");
static const espbt::ESPBTUUID NOTIFY_UUID = espbt::ESPBTUUID::from_raw("49535343-1e4d-4bd9-ba61-23c647249616");
static const espbt::ESPBTUUID WRITE_UUID = espbt::ESPBTUUID::from_raw("49535343-8841-43f4-a8d4-ecbe34729bb3");

static bool contains_ignore_case(const std::string &haystack, const std::string &needle) {
    if (needle.empty() || needle.size() > haystack.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               std::tolower((unsigned char) haystack[i + j]) == std::tolower((unsigned char) needle[j])) {
            ++j;
        }
        if (j == needle.size())
            return true;
    }
    return false;
}

std::vector<B06Component *> B06Component::instances_;

void B06Component::setup() {
    BLEClientBase::setup();
    instances_.push_back(this);
    bike_scan_v2::add_user(this);
    this->restore_last_state_();
    this->update_ble_status_();
}

void B06Component::dump_config() {
    ESP_LOGCONFIG(TAG, "B06 display:");
    for (const auto &name : this->device_names_)
        ESP_LOGCONFIG(TAG, "  Device name: %s", name.c_str());
    if (this->max_connection_ms_ != 0) {
        ESP_LOGCONFIG(TAG, "  Max connection time: %u s", (unsigned) (this->max_connection_ms_ / 1000));
    } else {
        ESP_LOGCONFIG(TAG, "  Max connection time: unlimited");
    }
    LOG_SENSOR("  ", "Battery", this->battery_sensor_);
    LOG_SENSOR("  ", "Speed", this->speed_sensor_);
    LOG_SENSOR("  ", "Max speed", this->max_speed_sensor_);
    LOG_SENSOR("  ", "Avg speed", this->avg_speed_sensor_);
    LOG_SENSOR("  ", "Trip distance", this->trip_distance_sensor_);
    LOG_SENSOR("  ", "Trip duration", this->trip_duration_sensor_);
    LOG_SENSOR("  ", "Odometer", this->odometer_sensor_);
    LOG_TEXT_SENSOR("  ", "Firmware version", this->firmware_version_text_sensor_);
    LOG_TEXT_SENSOR("  ", "BLE status", this->ble_status_text_sensor_);
    LOG_SWITCH("  ", "Light", this->light_switch_);
}

bool B06Component::matches_(const espbt::ESPBTDevice &device) {
    const uint64_t address = device.address_uint64();
    const bool known = std::find(std::begin(this->known_addresses_), std::end(this->known_addresses_), address) !=
                       std::end(this->known_addresses_);
    for (const auto &name : this->device_names_) {
        if (!contains_ignore_case(device.get_name(), name))
            continue;
        if (!known) {
            this->known_addresses_[this->known_address_next_] = address;
            this->known_address_next_ = (this->known_address_next_ + 1) % MAX_KNOWN_ADDRESSES;
        }
        return true;
    }
    // The display sometimes omits its name. Its service alone would also
    // match other people's displays and unrelated devices, so only an address
    // this block matched by name before is taken.
    if (!device.get_name().empty() || !known)
        return false;
    for (const auto &uuid : device.get_service_uuids()) {
        if (uuid == SERVICE_UUID)
            return true;
    }
    return false;
}

bool B06Component::claimed_by_other_(uint64_t address) const {
    for (const auto *other : instances_) {
        if (other == this)
            continue;
        if ((other->state() != espbt::ClientState::IDLE && other->get_address() == address) ||
            other->resting_address_ == address)
            return true;
    }
    return false;
}

bool B06Component::parse_device(const espbt::ESPBTDevice &device) {
    // A resting block only watches its own display; it takes no other until
    // that one has left, so no display stays connected without a time limit.
    if (this->resting_address_ != 0) {
        if (device.address_uint64() == this->resting_address_)
            this->resting_seen_ms_ = millis();
        return false;
    }
    // Before the GATT client is registered, connect() would only be rejected.
    if (this->gattc_if_ == ESP_GATT_IF_NONE || this->state() != espbt::ClientState::IDLE || !this->matches_(device) ||
        this->claimed_by_other_(device.address_uint64()))
        return false;

    char address[espbt::ESPBTDevice::MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    ESP_LOGI(TAG, "Found display '%s' [%s], connecting", device.get_name().c_str(), device.address_str_to(address));
    this->set_address(device.address_uint64());
    this->set_remote_addr_type(device.get_address_type());
    this->set_state(espbt::ClientState::DISCOVERED);
    return true;
}

void B06Component::set_state(espbt::ClientState state) {
    BLEClientBase::set_state(state);
    if (state == espbt::ClientState::IDLE)
        this->reset_session_();
    this->update_ble_status_();
}

void B06Component::connect() {
    this->connect_started_ms_ = millis();
    this->got_data_ = false;
    BLEClientBase::connect();
    // Scanning alongside the setup costs connections: it stays off until the
    // display is listened to or the session resets. An attempt that already
    // failed has reset the session.
    if (this->state() == espbt::ClientState::CONNECTING)
        bike_scan_v2::begin_setup(this);
}

bool B06Component::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
    if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
        return false;

    switch (event) {
        case ESP_GATTC_SEARCH_CMPL_EVT:
            this->on_services_ready_();
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (param->reg_for_notify.handle != this->tx_handle_)
                break;
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Subscribing to notifications failed (status=%d)", param->reg_for_notify.status);
                this->disconnect();
                break;
            }
            ESP_LOGD(TAG, "Subscribed %u ms after connecting, starting the authentication",
                     (unsigned) (millis() - this->connect_started_ms_));
            this->phase_ = Phase::HANDSHAKE;
            this->auth_sent_ = false;
            this->step_deadline_ = millis() + SUBSCRIBE_SETTLE_MS;
            // The base class enables notifications by writing this descriptor.
            auto *cccd = this->get_config_descriptor(this->tx_handle_);
            this->cccd_handle_ = cccd != nullptr ? cccd->handle : 0;
            this->cccd_confirmed_ = this->cccd_handle_ == 0;
            this->cccd_deadline_ = millis() + CCCD_CONFIRM_TIMEOUT_MS;
            this->update_ble_status_();
            break;
        }

        case ESP_GATTC_WRITE_DESCR_EVT:
            if (param->write.handle != this->cccd_handle_)
                break;
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "Enabling notifications failed (status=0x%02X), reconnecting", param->write.status);
                this->disconnect();
                break;
            }
            ESP_LOGD(TAG, "Notifications enabled");
            this->cccd_confirmed_ = true;
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            if (param->write.handle == this->rx_handle_ && param->write.status != ESP_GATT_OK)
                ESP_LOGW(TAG, "Display rejected a write (status=0x%02X)", param->write.status);
            break;

        case ESP_GATTC_NOTIFY_EVT: {
            if (param->notify.handle != this->tx_handle_)
                break;
            this->last_rx_ms_ = millis();
            auto &stats = this->link_stats_;
            ++stats.notifications;
            stats.notification_bytes += param->notify.value_len;
            this->log_bytes_("RX", param->notify.handle, param->notify.value, param->notify.value_len,
                             stats.notifications <= DEBUG_LOGGED);
            const uint32_t bad_crc_before = stats.bad_crc;
            const uint32_t unhandled_before = stats.unhandled;
            const bool changed = this->parser_.feed(this->state_, param->notify.value, param->notify.value_len,
                                                    stats.bad_crc, stats.unhandled);
            this->handle_auth_();
            if (stats.bad_crc != bad_crc_before)
                ESP_LOGD(TAG, "Frame failed the CRC check (%u this connection)", (unsigned) stats.bad_crc);
            if (stats.unhandled != unhandled_before)
                ESP_LOGD(TAG, "Received an unknown frame (%u this connection)", (unsigned) stats.unhandled);
            if (changed) {
                if (!this->got_data_) {
                    ESP_LOGI(TAG, "First data %u ms after connecting",
                             (unsigned) (millis() - this->connect_started_ms_));
                    this->got_data_ = true;
                }
                this->last_state_dirty_ = true;
                this->publish_state_();
            }
            break;
        }

        case ESP_GATTC_DISCONNECT_EVT:
        case ESP_GATTC_CLOSE_EVT:
            this->reset_session_();
            this->update_ble_status_();
            break;

        default:
            break;
    }
    return true;
}

void B06Component::on_services_ready_() {
    auto *tx = this->get_characteristic(SERVICE_UUID, NOTIFY_UUID);
    auto *rx = this->get_characteristic(SERVICE_UUID, WRITE_UUID);
    if (tx == nullptr || rx == nullptr) {
        ESP_LOGE(TAG, "No B06 service found (notify=%s write=%s)", YESNO(tx != nullptr), YESNO(rx != nullptr));
        this->log_services_();
        this->disconnect();
        return;
    }
    this->tx_handle_ = tx->handle;
    this->rx_handle_ = rx->handle;
    const esp_err_t err = this->register_for_notify(this->tx_handle_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register_for_notify failed: %d", err);
        this->disconnect();
    }
}

void B06Component::log_bytes_(const char *what, uint16_t handle, const uint8_t *data, size_t len, bool debug) {
#if ESPHOME_LOG_LEVEL < ESPHOME_LOG_LEVEL_VERBOSE
    if (!debug)
        return;
#endif
    char hex[format_hex_pretty_size(MAX_LOGGED_BYTES)];
    format_hex_pretty_to(hex, data, std::min(len, MAX_LOGGED_BYTES));
    if (debug) {
        ESP_LOGD(TAG, "%s 0x%04X, %u bytes: %s", what, handle, (unsigned) len, hex);
    } else {
        ESP_LOGV(TAG, "%s 0x%04X, %u bytes: %s", what, handle, (unsigned) len, hex);
    }
}

void B06Component::log_summary_() {
    this->last_summary_ = millis();
    const auto &s = this->link_stats_;
    ESP_LOGI(TAG, "%u requests, %u notifications (%u bytes), %u frames, %u bad CRC, %u unknown", (unsigned) s.requests,
             (unsigned) s.notifications, (unsigned) s.notification_bytes, (unsigned) this->parser_.frames(),
             (unsigned) s.bad_crc, (unsigned) s.unhandled);
}

// Lists what the device offers, so a log from an unsupported display shows
// which service it uses.
void B06Component::log_services_() {
    char uuid[esp32_ble::UUID_STR_LEN];
    for (auto *service : this->services_) {
        ESP_LOGW(TAG, "  Service %s", service->uuid.to_str(uuid));
        service->parse_characteristics();
        for (auto *chr : service->characteristics) {
            ESP_LOGW(TAG, "    Characteristic %s (properties 0x%02X)", chr->uuid.to_str(uuid), chr->properties);
        }
    }
}

void B06Component::loop() {
    BLEClientBase::loop();

    const uint32_t now = millis();
    this->flush_tx_(now);
    if (this->phase_ == Phase::HANDSHAKE) {
        this->run_handshake_(now);
        return;
    }
    // Until the close event resets the session, a disconnect is in progress.
    if (this->phase_ != Phase::LISTENING || !this->connected())
        return;

    if (now - this->last_summary_ >= SUMMARY_INTERVAL_MS)
        this->log_summary_();
    if (this->max_connection_ms_ != 0 && this->got_data_ &&
        now - this->listening_started_ms_ >= this->max_connection_ms_) {
        // Keeping the link open keeps the display from switching off.
        ESP_LOGI(TAG, "Read the display for %u s, disconnecting until it leaves",
                 (unsigned) ((now - this->listening_started_ms_) / 1000));
        this->resting_address_ = this->get_address();
        this->resting_seen_ms_ = now;
        // On the scheduler: the base class disables loop() once idle.
        this->set_interval("bike_gone", 1000, [this]() {
            if (millis() - this->resting_seen_ms_ < BIKE_GONE_MS)
                return;
            ESP_LOGI(TAG, "Display gone, connecting again when it is back");
            this->resting_address_ = 0;
            this->cancel_interval("bike_gone");
            this->update_ble_status_();
        });
        this->disconnect();
        return;
    }
    const uint32_t quiet_ms = now - this->last_rx_ms_;
    if (quiet_ms >= (this->got_data_ ? DATA_TIMEOUT_MS : FIRST_DATA_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "No data from the display for %u s, reconnecting", (unsigned) (quiet_ms / 1000));
        this->disconnect();
        return;
    }
    if (this->last_state_dirty_ && now - this->last_state_saved_ms_ >= LAST_STATE_SAVE_INTERVAL_MS)
        this->save_last_state_();
    if (this->state_.firmware[0] == '\0' && this->version_requests_ < VERSION_REQUEST_ATTEMPTS &&
        now - this->last_version_request_ >= VERSION_REQUEST_INTERVAL_MS)
        this->request_version_();
}

// Starts the authentication and finishes the handshake once the display has
// answered it; the challenge itself is answered from handle_auth_().
void B06Component::run_handshake_(uint32_t now) {
    if (!this->cccd_confirmed_) {
        if ((int32_t) (now - this->cccd_deadline_) < 0)
            return;
        ESP_LOGW(TAG, "Display did not confirm the notification subscription, continuing");
        this->cccd_confirmed_ = true;
    }
    if (!this->auth_sent_) {
        if ((int32_t) (now - this->step_deadline_) < 0)
            return;
        static const uint8_t START[] = {0x01};
        uint8_t frame[sizeof(START) + protocol::OVERHEAD];
        if (!this->send_frame_(frame, protocol::build_frame(frame, protocol::CMD_AUTH_START, START, sizeof(START)))) {
            ESP_LOGW(TAG, "Authentication request failed, disconnecting");
            this->disconnect();
            return;
        }
        this->auth_sent_ = true;
        this->step_deadline_ = now + AUTH_TIMEOUT_MS;
        return;
    }
    const auto result = this->parser_.auth_result();
    if (result == protocol::FrameParser::AuthResult::NONE && (int32_t) (now - this->step_deadline_) < 0)
        return;
    if (result == protocol::FrameParser::AuthResult::OK) {
        ESP_LOGI(TAG, "Display accepted the authentication");
    } else if (result == protocol::FrameParser::AuthResult::FAILED) {
        ESP_LOGW(TAG, "Display rejected the authentication, listening anyway");
    } else {
        ESP_LOGW(TAG, "No answer to the authentication, listening anyway");
    }
    this->finish_handshake_(now);
}

void B06Component::handle_auth_() {
    uint8_t challenge[protocol::AUTH_CHALLENGE_LEN];
    if (!this->parser_.take_challenge(challenge))
        return;
    uint8_t answer[16];
    uint8_t frame[sizeof(answer) + protocol::OVERHEAD];
    if (!protocol::auth_answer(challenge, answer)) {
        ESP_LOGW(TAG, "Computing the authentication answer failed");
        return;
    }
    ESP_LOGD(TAG, "Answering the authentication challenge");
    if (!this->send_frame_(frame, protocol::build_frame(frame, protocol::CMD_AUTH_ANSWER, answer, sizeof(answer))))
        ESP_LOGW(TAG, "Authentication answer failed");
}

void B06Component::finish_handshake_(uint32_t now) {
    this->phase_ = Phase::LISTENING;
    this->listening_started_ms_ = now;
    this->last_rx_ms_ = now;
    this->last_summary_ = now;
    this->request_version_();
    ESP_LOGI(TAG, "Listening to the display");
    this->update_ble_status_();
    // The setup is over: the scan may run again for bikes still searching.
    // The tracker only looks at the scan after a client state change.
    bike_scan_v2::end_setup(this);
    this->set_state_internal_(this->state());
}

void B06Component::request_version_() {
    this->last_version_request_ = millis();
    ++this->version_requests_;
    uint8_t frame[protocol::OVERHEAD];
    if (!this->send_frame_(frame, protocol::build_frame(frame, protocol::CMD_VERSION, nullptr, 0)))
        ESP_LOGW(TAG, "Version request failed");
}

// Queues a frame; flush_tx_() sends it. Returns false when the display is not
// connected or the queue is full.
bool B06Component::send_frame_(const uint8_t *frame, size_t len) {
    if (this->rx_handle_ == 0 || !this->connected() || len > MAX_WRITE_LEN || this->tx_count_ == TX_QUEUE_SIZE)
        return false;
    auto &slot = this->tx_queue_[(this->tx_head_ + this->tx_count_) % TX_QUEUE_SIZE];
    memcpy(slot.data, frame, len);
    slot.len = len;
    ++this->tx_count_;
    return true;
}

void B06Component::flush_tx_(uint32_t now) {
    if (this->tx_count_ == 0 || now - this->last_tx_ms_ < TX_SPACING_MS)
        return;
    auto &frame = this->tx_queue_[this->tx_head_];
    this->tx_head_ = (this->tx_head_ + 1) % TX_QUEUE_SIZE;
    --this->tx_count_;
    if (this->rx_handle_ == 0 || !this->connected())
        return;
    this->last_tx_ms_ = now;
    // Without response like the app, unless the frame does not fit one packet
    // (the 22-byte authentication answer at the default MTU of 23).
    const auto type = frame.len + 3u <= this->mtu_ ? ESP_GATT_WRITE_TYPE_NO_RSP : ESP_GATT_WRITE_TYPE_RSP;
    const esp_err_t err = esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->rx_handle_, frame.len,
                                                   frame.data, type, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Write failed: %d", err);
        return;
    }
    ++this->link_stats_.requests;
    this->log_bytes_("TX", this->rx_handle_, frame.data, frame.len, this->link_stats_.requests <= DEBUG_LOGGED);
}

bool B06Component::set_light(bool on) {
    if (this->phase_ != Phase::LISTENING) {
        ESP_LOGW(TAG, "Light command ignored: display not connected");
        return false;
    }
    uint8_t frame[4 + protocol::OVERHEAD];
    if (!this->send_frame_(frame, protocol::build_light(frame, on))) {
        ESP_LOGW(TAG, "Light write failed");
        return false;
    }
    ESP_LOGD(TAG, "Light %s", on ? "on" : "off");
    this->light_pending_ = true;
    this->light_pending_on_ = on;
    this->light_pending_until_ = millis() + LIGHT_CONFIRM_WINDOW_MS;
    return true;
}

void B06Component::reset_session_() {
    if (this->phase_ != Phase::IDLE)
        this->log_summary_();
    if (this->phase_ == Phase::LISTENING) {
        // Keep the last readings, but a speed from before the display left is
        // no longer current.
        if (this->state_.speed_kmh > 0)
            this->state_.speed_kmh = 0;
        this->publish_state_();
        this->last_state_dirty_ = true;
        this->save_last_state_();
        global_preferences->sync();  // disconnects are rare; do not wait for the periodic flush
    }
    this->phase_ = Phase::IDLE;
    this->auth_sent_ = false;
    this->got_data_ = false;
    this->light_pending_ = false;
    this->tx_count_ = 0;
    this->last_rx_ms_ = 0;
    this->version_requests_ = 0;
    this->tx_handle_ = 0;
    this->rx_handle_ = 0;
    this->cccd_handle_ = 0;
    this->cccd_confirmed_ = false;
    this->link_stats_ = {};
    this->parser_.reset();
    // Last, so the coordinator sees this display searching again: every way
    // out of a setup ends here. A resting display is idle too and keeps the
    // scan running, which is how it is seen to leave.
    bike_scan_v2::end_setup(this);
}

void B06Component::restore_last_state_() {
    this->last_state_pref_ =
        global_preferences->make_preference<LastState>(fnv1_hash("b06_last_state_" + this->storage_key_));
    LastState saved{};
    if (!this->last_state_pref_.load(&saved) || saved.magic != LAST_STATE_MAGIC)
        return;
    this->state_ = saved.state;
    ESP_LOGI(TAG, "Restored last known state (battery %d%%)", saved.state.battery);
    this->publish_state_();
}

void B06Component::save_last_state_() {
    if (!this->last_state_dirty_)
        return;
    LastState data{};
    data.magic = LAST_STATE_MAGIC;
    data.state = this->state_;
    if (this->last_state_pref_.save(&data)) {
        this->last_state_dirty_ = false;
        this->last_state_saved_ms_ = millis();
    } else {
        ESP_LOGW(TAG, "Saving last state failed");
    }
}

static void publish_if_changed(sensor::Sensor *sensor, float value) {
    if (sensor == nullptr || value < 0)
        return;
    if (!sensor->has_state() || sensor->state != value)
        sensor->publish_state(value);
}

void B06Component::publish_state_() {
    const auto &s = this->state_;
    publish_if_changed(this->battery_sensor_, s.battery);
    publish_if_changed(this->speed_sensor_, s.speed_kmh);
    publish_if_changed(this->max_speed_sensor_, s.max_speed_kmh);
    publish_if_changed(this->avg_speed_sensor_, s.avg_speed_kmh);
    publish_if_changed(this->trip_distance_sensor_, s.trip_distance_km);
    publish_if_changed(this->trip_duration_sensor_, s.trip_duration);
    publish_if_changed(this->odometer_sensor_, s.odometer_km);

    if (this->firmware_version_text_sensor_ != nullptr && s.firmware[0] != '\0' &&
        this->firmware_version_text_sensor_->state != s.firmware) {
        this->firmware_version_text_sensor_->publish_state(s.firmware);
    }
    if (this->light_switch_ != nullptr && s.light_valid) {
        if (this->light_pending_ &&
            (s.light_on == this->light_pending_on_ || (int32_t) (millis() - this->light_pending_until_) >= 0))
            this->light_pending_ = false;
        // Skip stale reports sent before the display applied the last command.
        if (!this->light_pending_ && this->light_switch_->state != s.light_on)
            this->light_switch_->publish_state(s.light_on);
    }
}

void B06Component::update_ble_status_() {
    const char *status;
    if (this->phase_ == Phase::LISTENING) {
        status = "connected";
    } else if (this->phase_ == Phase::HANDSHAKE) {
        status = "handshake";
    } else {
        switch (this->state()) {
            case espbt::ClientState::DISCOVERED:
            case espbt::ClientState::CONNECTING:
            case espbt::ClientState::CONNECTED:
            case espbt::ClientState::ESTABLISHED:
                status = "connecting";
                break;
            default:
                status = this->resting_address_ != 0 ? "paused" : "searching";
                break;
        }
    }
    if (status == this->last_ble_status_)
        return;
    this->last_ble_status_ = status;
    ESP_LOGD(TAG, "BLE status: %s", status);
    if (this->ble_status_text_sensor_ != nullptr)
        this->ble_status_text_sensor_->publish_state(status);
}

void B06LightSwitch::write_state(bool state) {
    // Optimistic: the display's next ride frame publishes the actual state.
    if (this->parent_->set_light(state)) {
        this->publish_state(state);
    } else {
        this->publish_state(this->state);
    }
}

}  // namespace esphome::b06

#endif  // USE_ESP32
