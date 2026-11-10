# BLE protocol

Reference for the protocol of the B06 display, reverse engineered from the BIKEGO plus app (version 1.6.1,
`com.huiye.b06`) and checked against a real display. Details may differ across display firmware versions.

## Service

The display advertises as `B06_BLE` and exposes the Microchip transparent UART service
(`49535343-fe7d-4ae5-8fa9-9fafd205e455`). Frames are written to `49535343-8841-…` and arrive as notifications
on `49535343-1e4d-…`.

## Frames

```
0xA5 [len] [cmd] [payload...] [CRC16_lo] [CRC16_hi] 0x0D
```

`len` counts the payload. The checksum is CRC-16/XMODEM (polynomial `0x1021`, initial value `0`) over `len` up
to the payload.

## Authentication

1. After subscribing, the bridge sends `20 01`.
2. The display answers `20` with a 4-byte challenge.
3. The bridge returns `21` with that challenge, zero-padded to 16 bytes and encrypted with AES-128-ECB under a
   key fixed in the app.
4. The display confirms with `21 00`.

The display then pushes its data without being polled.

## Commands

| Cmd | Payload | Decoding |
|---|---|---|
| `0x41` | ≥ 29 bytes | speed, max speed (uint16 LE ÷ 10), average speed (uint16 LE ÷ 100), two bytes the app calls range but never shows (not used), ride time (uint32 LE, s), trip and odometer (uint32 LE ÷ 100, km), then gear, light, mode, error code and other states as single bytes |
| `0x42` | ≥ 6 bytes | battery (%), then voltage, current (uint16 BE, raw) and charging |
| `0x40` | | version: a protocol type byte, then the software version as ASCII (request: empty payload) |
| `0x43` | `02 0E 01 [0\|1]` | switches the light (request) |

A real display sends longer frames than the app decodes (46 bytes for `41`, 99 for `42`); only the fields
above are used.

Other commands the app sends, which the bridge does not use: `44` clears the trip or the odometer, `4A` sets
the lock and its password and `4B` the speed limit password.
