# DMX Relay Node

DMX512 + Art-Net controlled 8-relay firmware for the Waveshare
[ESP32-S3-POE-ETH-8DI-8RO](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO).

<img src="docs/status.png" width="72%"> <img src="docs/phone.png" width="25%">

- PoE Ethernet (DHCP or static) + optional Wi-Fi
- DMX on RS485 and Art-Net on UDP 6454, each mapped to an 8-channel range
- Live status, manual relay test, web firmware update
- `http://dmx-relay.local/`

## Install

| | |
|---|---|
| **Update a running device** | Web UI → **System → Firmware update** → `firmware/dmx-relay-1.1.0.bin` |
| | or `curl -F "firmware=@firmware/dmx-relay-1.1.0.bin" http://<ip>/api/update` |
| **First flash / recovery (USB)** | `esptool --chip esp32s3 write-flash 0x0 firmware/dmx-relay-1.1.0-factory.bin` |
| **Build from source** | VS Code + PlatformIO → **Upload** (→), or `pio run -t upload` |

USB won't connect? Hold **BOOT**, tap **RESET**, release BOOT.

## Setup

1. Power via PoE (802.3af), USB-C or 7–36 V.
2. Browse to `http://dmx-relay.local/` or the DHCP address.
3. No network after 20 s → hotspot **`dmx-relay-XXXX`**, password **`dmxrelay`**, page at `192.168.4.1`.

Factory reset: hold **BOOT** 5 s.

## Pages

**Mapping:** DMX start address, Art-Net universe + start channel, relay → channel assignment, threshold, merge, signal loss.

<img src="docs/mapping.png" width="70%">

**Network:** Ethernet DHCP/static, Wi-Fi join + scan, hostname. Saving restarts the device.

<img src="docs/network.png" width="70%">

**System:** firmware info and update, restart, factory reset.

<img src="docs/system.png" width="70%">

## Behaviour

- Relay **on** when its channel ≥ threshold (default 128).
- Both inputs live → **HTP** (default), **LTP**, **Prefer DMX** or **Prefer Art-Net**.
- Signal lost (default 5 s) → **hold** (default) or **all off**. Relays start off at power-up.
- Art-Net universe = 15-bit port-address: `10` = Net 0 · Sub-Net 0 · Uni 10.

Test Art-Net: `python3 tools/artnet_test.py <ip> --universe 10`

## Status LED

| Teal | Dim blue | Flashing blue | Amber | Orange | Flashing red |
|---|---|---|---|---|---|
| Receiving | Online, idle | Setup hotspot | No network | Manual override | Relay driver fault |

## Wiring

RS485 **A** = DMX+ (XLR 3), **B** = DMX− (XLR 2). Fit the 120 Ω jumper on the last device. Receive only, no RDM.

## API

| | |
|---|---|
| `GET /api/status` | Full status |
| `GET /api/events` | Live updates (SSE) |
| `GET/POST /api/config` | Settings (partial JSON on POST) |
| `POST /api/relays` | `{"manual":true}`, `{"index":0,"on":true}`, `{"all":false}` |
| `GET /api/scan` | Wi-Fi scan |
| `POST /api/update` | Firmware upload |
| `POST /api/reboot`, `/api/factory-reset` | |

No authentication. Keep it on a trusted network.
