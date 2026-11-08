#pragma once

#ifdef USE_ESP32

#include <string>
#include <vector>

#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "protocol.h"
#include "scan_coordinator.h"

namespace esphome::b06 {

namespace espbt = esphome::esp32_ble_tracker;

// BLE client for one B06 display. Unlike the stock ble_client it needs no MAC
// address: it connects to the first advertiser whose name contains one of
// device_names, or that advertises no name but the display's service, and
// that no other block holds.
class B06Component : public esp32_ble_client::BLEClientBase, public bike_scan_v1::ScanUser {
 public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    bool parse_device(const espbt::ESPBTDevice &device) override;
    bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                             esp_ble_gattc_cb_param_t *param) override;
    void set_state(espbt::ClientState state) override;
    void connect() override;
    bool needs_scan() const override { return this->phase_ == Phase::IDLE; }

    // Key of this bike's state in flash; the block's id.
    void set_storage_key(const std::string &key) { this->storage_key_ = key; }
    void set_device_names(const std::vector<std::string> &names) { this->device_names_ = names; }
    // 0 keeps the bike connected for as long as it is in range.
    void set_max_connection_time(uint32_t ms) { this->max_connection_ms_ = ms; }

    void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
    void set_speed_sensor(sensor::Sensor *s) { this->speed_sensor_ = s; }
    void set_max_speed_sensor(sensor::Sensor *s) { this->max_speed_sensor_ = s; }
    void set_avg_speed_sensor(sensor::Sensor *s) { this->avg_speed_sensor_ = s; }
    void set_trip_distance_sensor(sensor::Sensor *s) { this->trip_distance_sensor_ = s; }
    void set_trip_duration_sensor(sensor::Sensor *s) { this->trip_duration_sensor_ = s; }
    void set_odometer_sensor(sensor::Sensor *s) { this->odometer_sensor_ = s; }
    void set_firmware_version_text_sensor(text_sensor::TextSensor *s) { this->firmware_version_text_sensor_ = s; }
    void set_ble_status_text_sensor(text_sensor::TextSensor *s) { this->ble_status_text_sensor_ = s; }
    void set_light_switch(switch_::Switch *s) { this->light_switch_ = s; }

    // Called by the light switch. Returns false when the bike is not connected.
    bool set_light(bool on);

 protected:
    enum class Phase : uint8_t { IDLE, HANDSHAKE, LISTENING };

    bool matches_(const espbt::ESPBTDevice &device) const;
    bool claimed_by_other_(uint64_t address) const;
    void on_services_ready_();
    void log_services_();
    void log_bytes_(const char *what, uint16_t handle, const uint8_t *data, size_t len, bool debug);
    void log_summary_();
    void run_handshake_(uint32_t now);
    void handle_auth_();
    void finish_handshake_(uint32_t now);
    void request_version_();
    bool send_frame_(const uint8_t *frame, size_t len);
    void flush_tx_(uint32_t now);
    void reset_session_();
    void restore_last_state_();
    void save_last_state_();
    void publish_state_();
    void update_ble_status_();

    // Every block, so blocks never connect to the same bike.
    static std::vector<B06Component *> instances_;

    std::string storage_key_;
    std::vector<std::string> device_names_;
    uint32_t max_connection_ms_{0};
    // The bike disconnected after max_connection_time, and when it last advertised.
    uint64_t resting_address_{0};
    uint32_t resting_seen_ms_{0};

    Phase phase_{Phase::IDLE};
    uint8_t handshake_step_{0};
    uint32_t next_step_at_{0};
    uint32_t connect_started_ms_{0};
    uint32_t listening_started_ms_{0};
    uint32_t last_rx_ms_{0};
    uint32_t last_summary_{0};
    uint32_t last_version_request_{0};
    uint8_t version_requests_{0};
    // Handshake waits until the bike confirms the notification subscription.
    uint16_t cccd_handle_{0};
    bool cccd_confirmed_{false};
    uint32_t cccd_deadline_{0};
    struct LinkStats {
        uint32_t requests{0};
        uint32_t notifications{0};
        uint32_t notification_bytes{0};
        uint32_t bad_crc{0};
        uint32_t unhandled{0};
    };
    LinkStats link_stats_;

    static constexpr size_t MAX_WRITE_LEN = 32;
    static constexpr size_t TX_QUEUE_SIZE = 4;
    struct TxFrame {
        uint8_t data[MAX_WRITE_LEN];
        uint8_t len;
    };
    TxFrame tx_queue_[TX_QUEUE_SIZE]{};
    uint8_t tx_head_{0};
    uint8_t tx_count_{0};
    uint32_t last_tx_ms_{0};
    // After a light command, light reports that still show the old state are
    // ignored until this deadline; the bike applies the change with a delay.
    uint32_t light_pending_until_{0};
    bool light_pending_on_{false};
    uint16_t tx_handle_{0};
    uint16_t rx_handle_{0};

    protocol::BikeState state_;
    // Last known telemetry, kept in flash so Home Assistant shows it after a
    // reboot instead of nothing.
    struct LastState {
        uint32_t magic;
        protocol::BikeState state;
    };
    ESPPreferenceObject last_state_pref_;
    bool last_state_dirty_{false};
    uint32_t last_state_saved_ms_{0};
    protocol::FrameParser parser_;
    const char *last_ble_status_{nullptr};

    sensor::Sensor *battery_sensor_{nullptr};
    sensor::Sensor *speed_sensor_{nullptr};
    sensor::Sensor *max_speed_sensor_{nullptr};
    sensor::Sensor *avg_speed_sensor_{nullptr};
    sensor::Sensor *trip_distance_sensor_{nullptr};
    sensor::Sensor *trip_duration_sensor_{nullptr};
    sensor::Sensor *odometer_sensor_{nullptr};
    text_sensor::TextSensor *firmware_version_text_sensor_{nullptr};
    text_sensor::TextSensor *ble_status_text_sensor_{nullptr};
    switch_::Switch *light_switch_{nullptr};
};

class B06LightSwitch : public switch_::Switch, public Parented<B06Component> {
 protected:
    void write_state(bool state) override;
};

}  // namespace esphome::b06

#endif  // USE_ESP32
