# DMX Relay Node — Waveshare ESP32-S3-POE-ETH-8DI-8RO

Custom firmware that turns the Waveshare [ESP32-S3-POE-ETH-8DI-8RO](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO)
into a DMX512 / Art-Net controlled 8-relay node, configured from a mobile-friendly web page.

- Powers from PoE, gets an address by DHCP on Ethernet (W5500), with an optional static IP
- Optional Wi-Fi station alongside Ethernet (DHCP or static), with network scan in the UI
- DMX512 input on the isolated RS485 port: set a start address, and the 8 channels from there drive the relays
- Art-Net input on UDP 6454 (Ethernet and Wi-Fi): set a universe and start channel, and the same relay mapping applies
- Per-relay channel assignment and names, a switch-on threshold, a merge mode for when both inputs are live,
  and hold-or-off behaviour on signal loss
- Live status page, manual override for testing, firmware upload, factory reset
- Answers Art-Net `ArtPoll`, so consoles list it as a node
- `http://dmx-relay.local/` through mDNS

## Build & flash

Requires [PlatformIO](https://platformio.org/). The project uses the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino-ESP32 3.3, ESP-IDF 5.5),
which has native W5500 support.

```sh
pio run -t upload        # build + flash over the USB-C port
pio device monitor       # serial log (USB CDC, 115200)
```

If upload fails to connect, hold **BOOT**, tap **RESET**, release BOOT, and retry.

After the first USB flash you can update over the network: **System → Firmware update**, using
`.pio/build/esp32-s3-poe-eth-8di-8ro/firmware.bin`.

The UI source is `web/index.html`. `scripts/embed_web.py` gzips it into `src/web_index.h` on every build.

## First setup

1. Plug in PoE. The device takes a DHCP lease and prints its IP on the serial log.
2. Open `http://dmx-relay.local/`, or the IP from your DHCP server's lease list.
3. If neither Ethernet nor Wi-Fi has an address after 20 s, the device opens a setup hotspot named
   **`dmx-relay-XXXX`** with password **`dmxrelay`**. Join it, and the setup page opens as a captive portal
   (or browse to `http://192.168.4.1/`). The hotspot closes 2 minutes after a network connects.

Hold **BOOT** for 5 s at any time to factory-reset. Settings go back to DHCP on Ethernet with Wi-Fi off.

## How mapping works

Each input listens to an 8-channel range:

| Setting | Example | Channels used |
|---|---|---|
| DMX start address | `101` | DMX 101–108 |
| Art-Net universe + start channel | `17`, `1` | universe 17 (Net 0 · Sub-Net 1 · Uni 1), ch 1–8 |

Each relay is assigned one of those 8 channels (or *Unassigned*). DMX and Art-Net share the assignment.
A relay closes when its channel is **≥ threshold** (default 128).

When both inputs are live:

- **HTP** (default): the higher value wins
- **LTP**: the input that updated most recently wins
- **Prefer DMX** / **Prefer Art-Net**: use the preferred input whenever it's live, and fall back to the other

An input counts as lost after the signal-loss timeout (default 5 s). When both inputs are lost, the relays
either **hold** their last state or all switch **off**. Relays always start off at power-up.

The Art-Net universe is the 15-bit Port-Address (`Net << 8 | SubNet << 4 | Universe`), so universe 0 is
`0:0:0` and universe 17 is `0:1:1`. The UI shows this breakdown.

## Status LED

| Colour | Meaning |
|---|---|
| Teal | DMX or Art-Net is being received |
| Dim blue | Online, no input |
| Flashing blue | Setup hotspot is active |
| Amber | No network yet |
| Orange | Manual override is on |
| Flashing red | Relay expander (TCA9554) not responding |

If red and green look swapped, change `LED_COLOR_ORDER_RGB` in `src/main.cpp`.

## Wiring notes

- DMX goes into the RS485 terminal: **A = Data+ (XLR pin 3)**, **B = Data− (XLR pin 2)**, GND to pin 1
  if your line needs it. If this node is the last device on the line, fit the board's 120 Ω termination jumper.
- The DMX input is receive-only. RDM is not implemented.

## Pins

| Function | GPIO |
|---|---|
| W5500 SCK / MISO / MOSI / CS / INT | 15 / 14 / 13 / 16 / 12 |
| I2C SDA / SCL (TCA9554 @ 0x20 → relays) | 42 / 41 |
| RS485 TX / RX (DMX) | 17 / 18 |
| WS2812 status LED | 38 |
| BOOT button | 0 |

## HTTP API

| Method | Path | Body / result |
|---|---|---|
| GET | `/api/status` | Network, input and relay state |
| GET | `/api/config` | Current settings (the Wi-Fi password is never returned) |
| POST | `/api/config` | Partial settings JSON (same shape as GET). Network changes reboot the device |
| POST | `/api/relays` | `{"manual":true}`, `{"index":0,"on":true}`, `{"all":false}` |
| GET | `/api/scan[?start=1]` | Wi-Fi scan; poll until `running` is false |
| POST | `/api/update` | multipart firmware upload |
| POST | `/api/reboot`, `/api/factory-reset` | |

The web UI and API have no authentication, so keep the device on a trusted lighting/control network.

## Source layout

| File | Purpose |
|---|---|
| `src/main.cpp` | Startup, status LED, factory-reset button |
| `src/dmx_rx.*` | DMX512 receiver (custom UART ISR with break detection) |
| `src/engine.*` | Art-Net listener, input merging, relay output task |
| `src/relays.*` | TCA9554 relay expander |
| `src/net.*` | Ethernet, Wi-Fi, setup hotspot, mDNS |
| `src/web.*` | Web server and JSON API |
| `src/config.*` | Settings, validation, NVS storage |
| `web/index.html` | The web UI |
