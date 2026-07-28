# ESP32-MIDI-WIFI

[![build](https://github.com/Fohdeesha/ESP32-MIDI-WIFI/actions/workflows/build.yml/badge.svg)](https://github.com/Fohdeesha/ESP32-MIDI-WIFI/actions/workflows/build.yml)

Firmware for the ESP32-S3 that turns any class-compliant USB MIDI device into a
wireless one. Plug a keyboard or controller into the ESP32-S3's USB OTG port
(host mode) and its MIDI events are bridged — in both directions — over WiFi
using RTP-MIDI (AppleMIDI, RFC 6295), so it shows up in macOS, Windows
(rtpMIDI), and Linux as a standard network MIDI session.

**Current version: 1.1.0**

## How it works

```
USB MIDI device (keyboard, controller, control surface)
      │  ▲  USB OTG host mode (ESP-IDF usb_host stack)
      ▼  │
USB MIDI class driver ◄──► MIDI event queues (FreeRTOS)
                                │  ▲
                                ▼  │
                     RTP-MIDI / AppleMIDI session
                     (UDP ports 5004/5005)
                                │  ▲  WiFi
                                ▼  │
                        DAW / network MIDI host
```

The bridge is fully bidirectional: notes and controls flow from the USB device
to the network, and network MIDI flows back to the device (LEDs, motorized
faders, displays on control surfaces). SysEx is chunked and reassembled
correctly in both directions. Virtual cable 0 is bridged.

## Features

- **USB host** for class-compliant USB MIDI devices, including workarounds for
  common descriptor quirks (e.g. devices that report a high-speed bulk packet
  size on a full-speed link).
- **Bidirectional RTP-MIDI bridge** hardened for sustained high-rate traffic
  (large parse buffer so no datagram straddles reads, deep TX queue with
  backpressure so device-bound SysEx bursts don't tear).
- **Session listener and initiator**: by default the device accepts incoming
  AppleMIDI invitations; optionally configure a peer IP:port and it will
  initiate (and re-invite every 30 s until connected).
- **Web config UI** (`http://esp32-midi.local/`): status, WiFi and network
  settings, RTP-MIDI session settings, password management, factory reset, and
  a live diagnostics view (USB state, decoded recent MIDI events, RTP-MIDI
  session event log, USB descriptor dump).
- **Static IP or DHCP** (DHCP by default), configurable from the web UI with
  validation.
- **OTA firmware updates** over HTTP — no serial connection needed once the
  device is on your network. Malformed or interrupted uploads are rejected
  safely, and a boot guard auto-rolls-back to the previous firmware if a new
  image crash-loops (3 failed boots).
- **Setup AP fallback**: if the configured WiFi is unreachable for 30 s, the
  device opens its own configuration access point while continuing to retry.
- **WS2812 status LED**: WiFi/session state and MIDI activity at a glance.
- **No baked-in credentials**: all configuration lives in NVS flash, never in
  the source or the binary.

## Hardware

- ESP32-S3 dev board (default target: `esp32-s3-devkitc-1` with 16 MB flash /
  8 MB PSRAM; adjust `board`/`board_build` in `platformio.ini` for other
  hardware).
- The S3's USB OTG peripheral (GPIO19/20) is used for host mode, so serial
  console and flashing go through the board's UART port — use the connector
  labeled **UART** on dual-port devkits.
- In host mode the board must supply 5 V VBUS to the attached device. Most
  devkits need a jumper or external 5 V feed for this (e.g. the USB-OTG pad on
  the DevKitC-1).
- Status LED is the devkit's onboard WS2812 on GPIO 48.

## Pre-built firmware

Every version is built by CI and published on the
[Releases](https://github.com/Fohdeesha/ESP32-MIDI-WIFI/releases) page, so
building from source is only necessary if you are changing the firmware. Each
release carries:

- `ESP32-MIDI-WIFI-<version>.bin` — the application image, for OTA updates.
- `ESP32-MIDI-WIFI-<version>-merged.bin` — a complete flash image for a blank
  board:

  ```sh
  esptool --chip esp32s3 write_flash 0x0 ESP32-MIDI-WIFI-<version>-merged.bin
  ```

- `bootloader.bin` and `partitions.bin`, if you prefer to flash the pieces
  separately (offsets 0x0 and 0x8000; the application goes at 0x10000).

## Building

Requires [PlatformIO](https://platformio.org/).

```sh
pio run                  # build
pio run -t upload        # first flash (via the UART port)
pio device monitor       # serial console, 115200 baud
```

Subsequent updates can go over the air instead:

```sh
pio run
curl -u admin:<password> -F "fw=@.pio/build/esp32-s3-devkitc-1/firmware.bin" http://esp32-midi.local/update
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

## Configuration

Everything is set from the web UI:

- **WiFi**: SSID (with a live scan list) and password.
- **RTP-MIDI session name** shown to network peers.
- **Connect to peer**: leave blank to accept incoming session invitations, or
  enter an IP (and port) to have the device initiate the session — useful when
  the other end is also a listener.
- **Static IP**: leave blank for DHCP, or set address, subnet mask, gateway
  (optional), and DNS (optional, defaults to the gateway). Inputs are
  validated before saving. Note that a wrong-but-valid static address can make
  the device unreachable; recovery is the BOOT-button factory reset below.
- **Web UI password**: HTTP Basic auth (username `admin`) guarding the page,
  config changes, OTA uploads, and factory reset. Default `midimidi`;
  changeable or removable.

Factory reset (erases all settings): button on the config page, or hold the
**BOOT button for 10 seconds** while the device is running — the recovery path
for a forgotten password or bad network config on a headless device.

## Version history

- 1.1.0 — static IP support (web-configurable, validated, DHCP remains the
  default)
- 1.0.0 — load-hardening after sustained high-traffic soak testing: fixed
  inbound-traffic starvation of session upkeep, deepened the USB TX queue with
  backpressure, enlarged the RTP parse buffer so datagram loss can't corrupt
  the session; added the session event log to the status page
- 0.8.0 — session initiator mode (invite a configured peer by IP:port)
- 0.7.0 — reverse direction: RTP-MIDI in → USB MIDI out (LEDs, faders,
  displays)
- 0.6.0 — full bridge: USB MIDI in → RTP-MIDI out
- 0.5.0 — USB host: enumerate USB MIDI devices, parse MIDI-over-USB packets
- 0.4.0 — web config UI + OTA firmware updates + setup-AP fallback
- 0.3.0 — RTP-MIDI session established
- 0.2.0 — WiFi station connect + mDNS advertisement + WS2812 status LED
- 0.1.0 — project skeleton
