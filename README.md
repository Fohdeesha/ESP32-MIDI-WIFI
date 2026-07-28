# ESP32-MIDI-WIFI

[![build](https://github.com/Fohdeesha/ESP32-MIDI-WIFI/actions/workflows/build.yml/badge.svg)](https://github.com/Fohdeesha/ESP32-MIDI-WIFI/actions/workflows/build.yml)

Firmware for the ESP32-S3 that turns any class-compliant USB MIDI device into a
wireless one. Plug a keyboard or controller into the ESP32-S3's USB OTG port
(host mode) and its MIDI events are bridged — in both directions — over WiFi
using RTP-MIDI (AppleMIDI, RFC 6295), so it shows up in macOS, Windows
(rtpMIDI), and Linux as a standard network MIDI session.

**Current version: 1.5.1**

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
correctly in both directions. Which of the device's MIDI ports is bridged is
configurable per direction (default: the first port, both ways).

## Features

- **USB host** for class-compliant USB MIDI devices, including workarounds for
  common descriptor quirks (e.g. devices that report a high-speed bulk packet
  size on a full-speed link).
- **Selectable MIDI port**: multi-port devices present several virtual cables
  (and occasionally several MIDI interfaces) — pick which one to bridge from
  the web UI, separately per direction if a device needs it, or merge every
  incoming port into one stream. The picker lists what the attached device
  actually presents, with live per-port event counters so you can tell which
  port your controller is really using, and warns if a selected port is beyond
  what the device declares in that direction.
- **Bidirectional RTP-MIDI bridge** hardened for sustained high-rate traffic
  (large parse buffer so no datagram straddles reads, deep TX queue with
  backpressure so device-bound SysEx bursts don't tear) and for low latency:
  the main loop is kept free of blocking calls, including in the upstream
  AppleMIDI library, and runs at ~480 Hz under a heavy bidirectional load.
- **Session listener and initiator**: by default the device accepts incoming
  AppleMIDI invitations; optionally configure a peer IP:port and it will
  initiate (and re-invite every 30 s until connected).
- **Web config UI** (`http://esp32-midi.local/`): status, WiFi and network
  settings, RTP-MIDI session settings, MIDI port selection, password
  management, factory reset, and a live diagnostics view in collapsible
  sections (USB state, decoded recent MIDI events in both directions with their
  port numbers, RTP-MIDI session event log, USB descriptor dump).
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

The build runs `tools/patch_applemidi.py` first, which removes a per-SysEx-byte
`Serial.print` from the AppleMIDI dependency — left-over debug code in its only
release, and enough to stall the main loop for hundreds of milliseconds under a
SysEx load (see 1.5.0 below). The script is idempotent and fails the build if
that code ever changes upstream, so the fix can't silently lapse.

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

- **WiFi**: SSID and password. The page lists nearby networks to pick from, but
  only scans while no MIDI session is connected — scanning takes the radio off
  its channel for several seconds, which would interrupt a live stream. During
  a session you can type the SSID, or use the "scan anyway" link and accept a
  brief dropout.
- **RTP-MIDI session name** shown to network peers.
- **Connect to peer**: leave blank to accept incoming session invitations, or
  enter an IP (and port) to have the device initiate the session — useful when
  the other end is also a listener.
- **USB MIDI interface**: which MIDI function of the attached device to claim.
  Almost every device has exactly one, so leave this on *Auto*; the list shows
  each MIDIStreaming interface the device presents and how many virtual cables
  it declares in each direction. A saved selection that isn't on the currently
  attached device falls back to the first usable one, and the status line says
  so rather than silently going quiet.
- **USB MIDI port, device → network**: which of the device's virtual cables
  (the "ports" a DAW would list — MIDI 1, MIDI 2, …) to forward to the
  network. Defaults to port 1. Ports the device declares are marked, and any
  port that has carried traffic shows its event count, so you can identify the
  right one even for devices whose descriptors understate what they have.
  *All ports* merges every incoming port into the single network stream.
- **USB MIDI port, network → device**: where network MIDI is sent on the
  device. Defaults to *same as the port above*, which is what a control
  surface needs — it expects its LEDs and faders back on the port it sent
  from. Set it explicitly for the two cases where the directions differ: a
  device that declares a different number of ports each way, and *All ports*
  in the other direction (a merged input has no single port for the return
  path to follow). The page warns if either selection is beyond what the
  device declares for that direction.

  Changes to any of the three take effect after the reboot that saving
  triggers.
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

- 1.5.1 — status page tidy-up: the three diagnostic logs (recent MIDI from the
  device, recent MIDI to the device, RTP-MIDI session events) are now
  collapsible sections that start closed, like the USB descriptor dump, so the
  status table stays visible instead of being pushed off the top of the page by
  a long session log. Log text is also rendered a little larger, the page picked
  up a red accent for its title and buttons, and there is now an author/project
  footer.
- 1.5.0 — **large latency fix.** Under a sustained device-bound SysEx load (a
  control surface's displays), the main loop was collapsing from ~480 Hz to
  around 4 Hz, with single iterations as long as 240 ms. Incoming MIDI was then
  delivered in four clumps a second instead of continuously, ~6% of outgoing
  RTP packets were lost to the resulting back-to-back bursts, and inbound
  display writes were dropped wholesale. The cause was in the upstream
  AppleMIDI library: its SysEx parser prints every SysEx byte to `Serial`, in
  code that is not behind any debug switch, and Arduino-ESP32 gives
  `HardwareSerial` no TX buffer — so each byte cost ~434 µs of blocking UART
  whether or not anything was listening on the port. One 945-byte datagram took
  206 ms to parse. The library is patched at build time (`tools/`), and the
  build fails rather than silently reshipping the stall if a future release
  changes that code. A per-event `Serial` log on the USB → network path was
  removed for the same reason. Measured after: loop max 49 ms and zero
  iterations over 100 ms across 1.07 M iterations, no RTP packet loss.
  Also: browsing the web UI no longer starts a WiFi scan while a MIDI session
  is connected — a scan takes the radio off-channel for seconds and interrupted
  the stream. The network list is still scanned during setup, and there's an
  explicit "scan anyway" link
- 1.4.0 — the bridged port is now selectable per direction. The network → device
  port defaults to following the device → network one (what a control surface
  needs), and can be set independently for the two cases where that isn't
  right: devices that declare a different number of ports in each direction,
  and "all ports" merged inbound, which leaves the return path no single port
  to follow. Each port list is annotated from the descriptors for its own
  direction, and the page warns when a selection is beyond what the device
  declares that way
- 1.3.0 — selectable MIDI port: which MIDI function of the attached device
  (MIDIStreaming interface) and which of its up to 16 virtual cables get
  bridged are now set from the web UI, instead of being fixed at the first
  interface and cable 0. The picker lists what the device actually presents —
  every MIDIStreaming interface with its declared cable counts, plus live
  per-port event counters so a device whose descriptors understate its ports
  can still be configured from observed traffic — and "All ports" merges every
  port device → network. A saved selection that is absent on the attached
  device falls back to the first usable interface rather than leaving the
  bridge silently dead; send-only devices (no MIDI IN endpoint) are now
  supported; and the status page gained a device-bound MIDI event log next to
  the existing inbound one, both labelled with the port each message is on
- 1.2.0 — session health: the bridge now sends MIDI Active Sensing (0xFE)
  every 250 ms while (and only while) its USB device is attached and
  demonstrably working — IN pipeline live, OUT transfers ACKing — so a host
  can treat its absence as "bridge or device gone" within a second instead
  of waiting out a session timeout; explicit device attach/detach status
  messages (sysex F0 7D 55 4D 42 <state> F7); a deaf-but-enumerated USB
  pipeline now counts as unhealthy instead of silently looking fine; and on
  session loss the bridge blanks a Mackie-Control-family surface (displays,
  LEDs, meters) so a frozen display can't masquerade as a live one
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
