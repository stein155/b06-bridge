# Support

Community project with best-effort support. Before opening an issue:

- Update to the latest commit on `main` and flash it.
- Restart the ESP32 and the display, and close the BIKEGO plus app: a display accepts one connection.
- Check the logs (`esphome logs`, the ESPHome dashboard, or USB serial at `115200` baud).
- Check that the display's Bluetooth name matches `device_names`; the default is `B06`, and the display
  advertises as `B06_BLE`.
- A BLE status of `paused` is expected after `max_connection_time`: the bridge leaves the display alone until
  it has not advertised for a minute.

Include in a bug report: the commit, ESPHome and Home Assistant versions, ESP32 board, bike and display, and
logs from boot through the failure.
