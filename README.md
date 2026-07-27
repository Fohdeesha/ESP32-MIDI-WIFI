# ESP32-MIDI-WIFI

Firmware for the ESP32-S3 that turns any class-compliant USB MIDI device into a
wireless one. Plug a keyboard or controller into the ESP32-S3's USB OTG port
(host mode) and its MIDI events are streamed over WiFi using RTP-MIDI
(AppleMIDI, RFC 6295), so it shows up in macOS, Windows (rtpMIDI), and Linux as
a standard network MIDI session.

**Current version: 0.4.5**

## How it works

```
USB MIDI device
      │  USB OTG host mode (ESP-IDF usb_host stack)
      ▼
USB MIDI class driver ──► MIDI event queue (FreeRTOS)
                                │
                                ▼
                     RTP-MIDI / AppleMIDI session
                     (UDP ports 5004/5005)
                                │  WiFi
                                ▼
                        DAW / network MIDI host
```

## Hardware

- ESP32-S3 dev board (default target: `esp32-s3-devkitc-1`; change `board` in
  `platformio.ini` for other hardware).
- The S3's USB OTG peripheral (GPIO19/20) is used for host mode, so serial
  console and flashing go through the board's UART port — use the connector
  labeled **UART** on dual-port devkits.
- In host mode the board must supply 5 V VBUS to the attached device. Most
  devkits need a jumper or external 5 V feed for this (e.g. the USB-OTG pad on
  the DevKitC-1).

## Building

Requires [PlatformIO](https://platformio.org/).

```sh
pio run                  # build
pio run -t upload        # first flash (via the UART port)
pio device monitor       # serial console, 115200 baud
```

## First-time setup

The firmware ships with no credentials baked in. On first boot (or after a
factory reset) the device opens a WiFi access point:

- **SSID:** `ESP32-MIDI-Setup` · **password:** `midimidi`
- Join it and browse to `http://192.168.4.1`
- Log in with the default credentials: username **admin**, password **midimidi**
  (changeable or removable on the config page)
- Enter your WiFi credentials and save; the device reboots onto your network
  and is reachable at `http://esp32-midi.local/`

The web UI also provides status, RTP-MIDI settings, access-password
management, over-the-air firmware updates, and factory reset (also triggered
by holding the BOOT button for 10 seconds). If the configured network is
unreachable for 30 seconds, the setup AP reopens automatically.

## Roadmap

- [x] 0.1.0 — project skeleton: builds, boots, prints version
- [x] 0.2.0 — WiFi station connect + mDNS advertisement + WS2812 status LED
- [x] 0.3.0 — RTP-MIDI session established, test notes sent
- [x] 0.4.0 — web config UI + OTA firmware updates + setup-AP fallback
- [ ] 0.5.0 — USB host: enumerate USB MIDI device, parse MIDI-over-USB packets
- [ ] 0.6.0 — full bridge: USB MIDI in → RTP-MIDI out
- [ ] 0.7.0 — reverse direction (RTP-MIDI in → USB MIDI out)
- [ ] 0.8.0 — session initiator mode (invite a configured peer by IP:port)
