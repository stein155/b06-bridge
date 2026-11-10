# B06 Bridge

ESPHome component that turns an ESP32 into a Bluetooth bridge between a B06 e-bike display (set up with the
BIKEGO plus app, for example with a Bafang BBS01 mid-drive) and [Home Assistant](https://www.home-assistant.io).
It connects to the display on its own, with no pairing or MAC address, and publishes battery, speed, trip,
odometer and the light as Home Assistant entities.

> Unofficial community project, not affiliated with or endorsed by any bike or display maker. Use at your
> own risk.

## Install

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/stein155/b06-bridge
      path: esphome/components
    components: [b06]

b06:
  max_connection_time: 30s
```

See [`esphome/b06-bridge.yaml`](esphome/b06-bridge.yaml) for the complete configuration with all entities.
Each display takes one of the ESP32's three BLE connections; add more `b06` blocks with their own `id` for
more displays.

## Next to other bike components

The component runs on the same ESP32 as other BLE bike components, such as
[tenways-bridge](https://github.com/stein155/tenways-bridge). Add both `external_components` entries and both
blocks. `scan_coordinator.h` is shared between them: scanning keeps running as long as any bike of any
component is still searching. It stops only while a bike is being set up, from connecting until the display is
listened to, and once all of them are connected.

## Connection time

The B06 display stays on while a client is connected. `max_connection_time` disconnects after reading it for
that long; the bridge then leaves the display alone until it has not advertised for a minute, that is after it
was switched off or ridden away. The BLE status entity shows `paused` meanwhile.

## Protocol

The display exposes the Microchip transparent UART service (`49535343-fe7d-4ae5-8fa9-9fafd205e455`). Frames
are written to `49535343-8841-…` and arrive as notifications on `49535343-1e4d-…`:

```
0xA5 [len] [cmd] [payload...] [CRC16_lo] [CRC16_hi] 0x0D
```

`len` counts the payload; the checksum is CRC-16/XMODEM (polynomial `0x1021`, initial value `0`) over `len` up
to the payload. After subscribing, the bridge sends `20 01`; the display answers `20` with a 4-byte challenge,
and the bridge returns `21` with that challenge, zero-padded to 16 bytes and encrypted with AES-128-ECB under a
key fixed in the app. The display confirms with `21 00`. It then pushes its data without being polled:

| Cmd | Payload | Decoding |
|---|---|---|
| `0x41` | ≥ 29 bytes | speed, max speed (uint16 LE ÷ 10), average speed (uint16 LE ÷ 100), two bytes the app calls range but never shows (not used), ride time (uint32 LE, s), trip and odometer (uint32 LE ÷ 100, km), then gear, light, mode, error code and other states as single bytes |
| `0x42` | ≥ 6 bytes | battery (%), then voltage, current (uint16 BE, raw) and charging |
| `0x40` | | version: a protocol type byte, then the software version as ASCII (request: empty payload) |
| `0x43` | `02 0E 01 [0\|1]` | switches the light (request) |

The display advertises as `B06_BLE`. Other commands the app (version 1.6.1, `com.huiye.b06`) sends: `44`
clearing trip or odometer, `4A` the lock and its password and `4B` the speed limit password. A real display
sends longer frames than the app decodes (46 bytes for `41`, 99 for `42`); only the fields above are used.

## Development

`tests/run.sh` runs the host tests of the protocol code; it needs mbedtls (`libmbedtls-dev`, or
`brew install mbedtls@3`). Copy `esphome/secrets.yaml.example` to `esphome/secrets.yaml` and compile
`esphome/dev.yaml` with ESPHome 2026.10.0.

`./lint.sh` runs clang-format, ruff, yamllint and shellcheck, as CI does; install them with
`pip install -r requirements-lint.txt`, and pass `--fix` to apply the fixes.

## License

MIT
