# Contributing

Contributions are welcome. This is a small, independent hobby project.

## Layout

- `esphome/components/b06/`: the ESPHome component (C++ and Python)
- `esphome/components/b06/scan_coordinator.h`: scan pausing shared with other bike components; keep it identical
  to the copies in those repositories
- `esphome/b06-bridge.yaml`: the complete example configuration
- `esphome/dev.yaml`: development configuration
- `docs/protocol.md`: protocol reference
- `tests/`: host tests of the protocol code

## Build and test

Install [ESPHome](https://esphome.io/guides/installing_esphome) at the version pinned in
`.github/workflows/esphome.yml`, then:

```bash
tests/run.sh                                            # host tests, needs mbedtls
pip install -r requirements-lint.txt && ./lint.sh       # the lint gate CI runs; --fix applies fixes
cp esphome/secrets.yaml.example esphome/secrets.yaml    # fill in WiFi and keys
esphome run esphome/dev.yaml                            # compile and flash over USB or WiFi
esphome logs esphome/dev.yaml                           # live logs over WiFi
```

Only the protocol code has automated tests. Verify everything else on hardware: the logs, the BLE
connection and the Home Assistant entities. CI runs the linters and the host tests, and compiles
`esphome/dev.yaml`.

## Pull requests

- Keep changes scoped and explain the *why*.
- Note the manual verification you did and on which display.
- Keep the entity keys (`battery`, `speed`, `max_speed`, `avg_speed`, `trip_distance`, `trip_duration`,
  `odometer`, `firmware_version`, `ble_status`, `light`) stable; they are the Home Assistant contract.

## Issues

Use the bug report template and include the commit, board, display and logs. See `SUPPORT.md` first.
