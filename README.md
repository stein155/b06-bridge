<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/hero-dark.svg">
    <img src="docs/assets/hero-light.svg" alt="B06 Bridge. Your e-bike. Now in Home Assistant. The B06 display of a Bafang mid-drive kit, without reaching for your phone. A B06 display, an ESP32 and Home Assistant in a row.">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/stein155/b06-bridge/actions/workflows/esphome.yml"><img alt="CI" src="https://github.com/stein155/b06-bridge/actions/workflows/esphome.yml/badge.svg?branch=main"></a>
  <img alt="ESPHome 2026.10" src="https://img.shields.io/badge/ESPHome-2026.10-111111?logo=esphome&logoColor=white">
  <img alt="ESP32" src="https://img.shields.io/badge/ESP32-ESP--IDF-111111?logo=espressif&logoColor=white">
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-111111"></a>
</p>

<p align="center">
  <a href="#setup"><b>Setup</b></a> ·
  <a href="#how-it-works"><b>How it works</b></a> ·
  <a href="#troubleshooting"><b>Troubleshooting</b></a> ·
  <a href="docs/protocol.md"><b>Protocol</b></a> ·
  <a href="CONTRIBUTING.md"><b>Contribute</b></a>
</p>

---

An ESPHome component for the **B06 display** that comes with Bafang mid-drive kits such as the BBS01. The
display normally only talks to the BIKEGO plus app, and only while your phone is next to the bike. B06 Bridge
puts an ESP32 near where the bike is parked instead: switch the display on and the bridge connects by
itself, with no pairing and no MAC address, and Home Assistant gets the battery, the odometer, the trip and
the light.

> [!NOTE]
> Unofficial community project, not affiliated with or endorsed by Bafang or any display maker. Use at your
> own risk.

## The story

I converted a cargo bike into an e-bike with a Bafang BBS01 mid-drive motor, and the kit came with a B06
display. My Tenways was already in Home Assistant through
[Tenways Bridge](https://github.com/stein155/tenways-bridge), and I wanted this one there too. Because why
not.

So I reverse engineered the display's Bluetooth protocol and built a component for it that runs on the same
ESP32. When the bike connects on departure or arrival, the bridge reads the latest data and reports it to
Home Assistant.

## What you get

| | |
| --- | --- |
| 🔋 **Battery** | Charge in percent, kept after the display switches off and after the bridge restarts. |
| 🛣️ **Odometer** | Total distance, as a `total_increasing` sensor for long-term statistics. |
| ⏱️ **Trip** | Speed, maximum and average speed, trip distance and trip duration. |
| 💡 **Light** | A switch that turns the bike's light on and off. |
| 🏷️ **Diagnostics** | Firmware version and the BLE status (`searching`, `connecting`, `handshake`, `connected`, `paused`). |

## Setup

You need an **ESP32** board with Bluetooth, such as an ESP32 DevKit (`esp32dev`), placed within Bluetooth
range of where the bike is parked and in reach of your WiFi, and [ESPHome](https://esphome.io) 2026.10 or
newer. Nothing goes on the bike: the bridge only listens over Bluetooth.

Add the component to the ESP32's configuration:

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

[`esphome/b06-bridge.yaml`](esphome/b06-bridge.yaml) is the complete configuration with all entities. Flash
it, add the bridge in Home Assistant under *Settings → Devices & services*, and switch the display on near
the ESP32: the BLE status goes to `connected` and the entities fill in.

| Option | Default | |
| --- | --- | --- |
| `device_names` | `[B06]` | Part of the Bluetooth name to look for; the display advertises as `B06_BLE`. |
| `max_connection_time` | `unlimited` | How long to read the display before letting it go; at least `5s`. |

## How it works

- **Discovery without pairing.** The bridge connects to the display whose Bluetooth name matches. Once found
  by name, it is also recognised when it advertises its service without a name.
- **Authentication.** The display sends a challenge, which the bridge answers the way the app does. After
  that the display pushes its data by itself. The [protocol reference](docs/protocol.md) has the details.
- **Lets the display switch off.** The display stays on while something is connected to it. With
  `max_connection_time` the bridge disconnects after reading it for that long, and leaves it alone until it
  has not advertised for a minute, that is after it was switched off or ridden away. The BLE status shows
  `paused` meanwhile.

### Next to Tenways Bridge

B06 Bridge runs on the same ESP32 as [Tenways Bridge](https://github.com/stein155/tenways-bridge): add both
`external_components` entries and both blocks to one configuration. They share the radio through the same
`scan_coordinator.h`: scanning keeps running while any bike is still searching, and stops while a bike is
being set up and once all of them are connected.

## Troubleshooting

| BLE status | What it means | What to try |
| --- | --- | --- |
| stays `searching` | The bridge does not see the display. | Switch the display on near the bridge. Close the BIKEGO plus app: the display accepts one connection. |
| `connecting` or `handshake`, then back to `searching` | The display is seen, but the connection does not hold. | Move the ESP32 closer to the bike, and away from metal and other 2.4 GHz devices. |
| `paused` | Expected after `max_connection_time`. | Nothing: the bridge reads the display again once the bike has been away. |

The logs tell the rest: `esphome logs <config> --device <bridge-ip>`, the ESPHome dashboard, or USB serial at
115200 baud. [SUPPORT.md](SUPPORT.md) lists what to include in a bug report.

## Development

The component lives in [`esphome/components/b06/`](esphome/components/b06), and the
[protocol reference](docs/protocol.md) documents what it speaks. The protocol code has host tests, so you do
not need the bike to work on it:

```bash
tests/run.sh                          # host tests of the protocol code (C++17 and mbedtls)
./lint.sh                             # clang-format, ruff, yamllint, shellcheck (requirements-lint.txt)
esphome compile esphome/dev.yaml      # copy esphome/secrets.yaml.example first
esphome logs esphome/dev.yaml --device <bridge-ip>
```

CI runs the linters and the host tests, and compiles `esphome/dev.yaml`. Everything outside the protocol
code is checked on the real display. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Built on [ESPHome](https://esphome.io) and [Home Assistant](https://www.home-assistant.io).
*Bafang* and the product names are trademarks of their owners, used only to identify the display the bridge
works with.
