# ESP32_DMX_WIFI

DMX / Art-Net relay node for the Waveshare ESP32-S3-POE-ETH-8DI-8RO (ESP32-S3, 16 MB flash, 8 MB PSRAM).
PlatformIO + Arduino-ESP32 3.x via pioarduino (ESP-IDF v5.5.5 underneath).

## Builds

- `pio run -e esp32-s3-poe-eth-8di-8ro`: standard relay node.
- `pio run -e animatronic`: adds `-DANIMATRONIC`. Relays 1/2 drive a reversing head motor with a DI1 center
  switch, relay 3 the eyes, plus four WAV sound slots played through a USB speaker (`src/sfx.cpp`).
- Both builds must keep compiling. USB audio code is animatronic-only (`lib_ignore = usb_host_uac` in the base env).
- `scripts/embed_web.py` gzips `web/index.html` into `src/web_index.h` before each build (generated, gitignored).
- Prebuilt binaries live in `firmware/` as `<env>-<version>.bin` (OTA via System page) and `-factory.bin`
  (full flash at 0x0). After an animatronic build, copy `.pio/build/animatronic/firmware.bin` and
  `firmware.factory.bin` there. The version string is still 1.1.0.

## Channels and settings

- `INPUT_COUNT` (16) input channels per range, separate from `RELAY_COUNT` (8). Channel slots are 1-16, 0 = unassigned.
- In the animatronic build each channel drives at most one output; `configMappingExclusive()` checks it on save.
  New mappable outputs need a claim there, a key in `web.cpp`'s mapping check, and entries in the UI's
  `chanKeysFrom()` / `outputGroups()` / save body.
- New settings fields with non-zero defaults: zero them in `configLoad()` before applying the stored JSON, so
  devices with saved settings don't get a default that collides with their mapping. Factory defaults reach
  existing devices only through the Mapping page's Restore defaults (`GET /api/config?defaults=1`).
- `g_cfgRev` bumps on every save and goes out in status, so open pages reload settings saved elsewhere.

## USB audio (animatronic)

- `lib/usb_host_uac` is Espressif's UAC host driver v1.5.0, vendored. Its Kconfig values are `-D` flags in the
  animatronic env. Local additions (listed in its README): `uac_host_device_get_tx_stats()` (per-packet
  isochronous OUT counters, bytes sent, packets with non-zero payload) and `uac_host_device_set_ep_frequency()`.
- The board's USB-C port supplies no 5 V. The speaker needs 5 V injected with a powered OTG adapter.
  Switching the port to USB host disables the USB serial monitor, so use the web Log page for diagnostics.
- Task priorities matter. The mixer (prio 6) holds `spkLock` almost continuously, so `uac_events` runs at
  prio 7. At a lower priority it starved on the mutex.

## Open issue: USB speakers are silent (as of 2026-10-27)

Symptom: the speaker enumerates and streams, but no sound comes out. The same speaker, OTG adapter and
charger play fine on a Mac.

Speakers tested, both silent: HONKYOB USB mini speaker (Jieli "UACDemoV1.0", 4c4a:4155) and a second
unit, 5a47:5858. Both have byte-identical descriptors from the same Jieli template, so they count as one
data point, not two. Layout: UAC 1.0, AC interface 0, Feature Unit 2 (mute on master, volume on ch 1-2),
streaming interface 1 alt 1, adaptive isochronous OUT EP 0x03, MPS 196/192, one format (48 kHz 16-bit
stereo), plus a HID interface. No buttons on the speaker.

Verified on the ESP32 (Log page):
- The 48 kHz stereo format is chosen. SET_INTERFACE alt 1, SET_CUR sampling frequency (accepted even
  though the descriptor doesn't advertise it), unmute and volume all succeed. Read back: mute off, -0.9 dB.
  These match what macOS reports through CoreAudio while the speaker plays.
- The bus carries 1000 packets/s at the full 192 kB/s with 0 skipped, and payloads are non-zero during
  the test tone. This holds with 3 packets per URB and with 1 (now `CONFIG_UAC_NUM_PACKETS_PER_URB=1`).
- A powered hub in between changes nothing.
- The ESP-IDF v5.5.5 HCD sends isochronous packets as DATA0, per the spec.
- Re-sending the controls while streaming, and cycling alt 0 then alt 1 after connect, didn't help.

Jieli SDK reference (github.com/Jieli-Tech/fw-AC63_BT_SDK, apps/common/device/usb/device/uac1.c and
uac_stream.c): SET_INTERFACE alt 1 opens the speaker stream and the ISO OUT handler feeds a cbuf that
the app plays. Nothing found that gates playback on another request.

Next step: run `tools/uac_replay` on a Linux box. It replays the firmware's exact sequence and stream from
a PC. If it plays, the fault is the ESP32 port's bus behaviour or timing. If it's silent, iterate on the
sequence there, which is faster than reflashing. Alternatively, test a non-Jieli device on the ESP32,
such as a C-Media CM108/CM119 USB sound card.
**Never run libusb driver-detach tools against a USB audio device on macOS. That kernel-panicked the Mac.**

### Second issue: speaker not detected at boot

After any ESP32 restart with the speaker plugged in, the log shows `HUB: Root port reset failed` and the
speaker never appears until it's reseated. Cycling root-port power in software
(`usb_host_lib_set_root_port_power`) doesn't help, because the port's 5 V isn't switched by the board.
The firmware still retries 6 times. Likely fix: switch the speaker's 5 V through a spare relay (4-8) and
power-cycle it from firmware when no device answers. Not built yet, waiting on which relay to use.

### Diagnostics in the firmware (keep or trim once resolved)

- Sounds page "Test tone" button: 3 s at 440 Hz and -6 dBFS, bypasses the volume and the WAV files
  (`sfxTone()`, `POST /api/sfx {"tone":1}`).
- Log lines once per connection: `Speaker stream: ...` 10 s after connect, `First non-silent packet on the bus`,
  `Cycled the stream interface ...`, `Re-sent unmute ... speaker reports mute=..., dB`,
  `Sampling frequency request ...`.
- Experiments to remove if they turn out irrelevant: interface cycle and control re-send after connect,
  unconditional sampling-frequency request, root-port re-power loop, `CONFIG_UAC_NUM_ISOC_URBS=6` /
  `PACKETS_PER_URB=1`.
