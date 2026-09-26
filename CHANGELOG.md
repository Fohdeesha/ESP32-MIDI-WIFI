# Changelog

## 1.7.3

- TX power now defaults to 8.5 dBm and tops out at 11 dBm. On some ESP32-S3
  boards the regulator can't keep up with the radio's transmit current, and at
  higher power that was enough to make WiFi fall apart under a steady stream of
  MIDI. A setting above 11 dBm saved by an earlier 1.7 release resets to
  8.5 dBm. New 7, 5 and 2 dBm options.
- WiFi reconnects back off instead of retrying instantly: the first retry comes
  after 250 ms, then the wait doubles up to 4 s. Some access points refuse a
  client for a while after it drops off without saying goodbye, and the instant
  retries turned that into hundreds of join attempts.
- During dense streams, MIDI headed to the network goes out in fewer, fuller
  packets, at most one every 10 ms. Single notes, chords and button presses
  still go out right away, and nothing is dropped or reordered.
- The status page and `/diag` show how many packets outgoing MIDI went out in,
  and how many reconnect attempts were made.

## 1.7.2

- A DAW that disappears without closing its session (laptop asleep, crashed,
  out of range) is dropped after 150 s instead of staying "connected" forever.
- Long SysEx arrives intact in both directions.
- MIDI clock, transport and timecode are passed through.
- USB transfer errors recover on their own instead of needing a replug.
- Short WiFi dropouts no longer open the setup hotspot.
- Power cycling can't roll the firmware back anymore, and a hung bridge
  restarts itself.
- The web UI refuses requests from other websites. Settings can only be changed
  with the page opened as esp32-midi.local or by IP address.

## 1.7.1

- A USB device that fails to enumerate is retried. ESP-IDF 4.4 only tries once
  per connection, so a self-powered device that hit a bad first attempt was
  never picked up again.
- New "USB port" status row (nothing there / detected but not enumerated /
  attached) and a log of the USB driver's own errors.

## 1.7.0

- Every WiFi disconnect is logged with its reason code, and the status page
  keeps a short history with the last known signal strength. `/diag` also
  reports why the chip last restarted (power-on, brownout, crash).
- A station that stays disconnected for 15 s gets a forced reconnect.
- TX power became a setting (defaulted to 19.5 dBm, lowered in 1.7.3).

## 1.6.3

- A Note On with velocity 0 is passed through as-is instead of being turned
  into a Note Off. Some control surfaces ignore Note Off for their LEDs, so
  lamps turned on and never went off.

## 1.6.2

- Resetting the diagnostic counters no longer zeroes the running totals on the
  status page.
- `/diagreset` only accepts POST.

## 1.6.1

- The Reboot button moved next to Save & reboot.

## 1.6.0

- New OUT pipeline stats on the status page (USB transfer latency, queue
  high-water mark, stalls), showing how close device-bound traffic runs to the
  limit.
- New plain-text `/diag` endpoint that's cheap to poll, and `/diagreset`.

## 1.5.4

Not released on its own; these changes shipped in 1.6.0.

- Four USB IN transfers in flight instead of two.
- The session event log no longer fills up with invite retries.
- TX power is set before connecting, so association can't reset it.

## 1.5.3

- The USB IN pipeline no longer shrinks after transfer errors. Each error used
  to retire a transfer for good, so MIDI from the device arrived in bunches and
  eventually stopped.
- New IN pipeline stats row.

## 1.5.2

- Reboot button on the config page.

## 1.5.1

- The diagnostic logs on the status page are collapsible.

## 1.5.0

- Fixed MIDI arriving in clumps under heavy SysEx traffic. The AppleMIDI
  library printed every SysEx byte to the serial port, which could block the
  main loop for 200 ms at a time. It's now patched at build time.
- Viewing the web page no longer starts a WiFi scan during a session.

## 1.4.0

- The bridged USB port can be set separately for each direction.

## 1.3.0

- Choose which USB MIDI interface and port to bridge, with live per-port event
  counts, or merge all ports.

## 1.2.0

- Active Sensing heartbeat every 250 ms while the USB device is working,
  attach/detach messages (SysEx `F0 7D 55 4D 42 01/00 F7`), and control
  surfaces are blanked when the session ends.

## 1.1.0

- Static IP support.

## 1.0.0

- Fixes for sustained heavy traffic: the session no longer starves under
  inbound load, a deeper USB send queue, and a bigger RTP parse buffer so a
  lost packet can't corrupt the session. Session event log on the status page.

## 0.8.0

- Can start the session itself by inviting a configured IP and port.

## 0.7.0

- Network to USB direction (LEDs, motor faders, displays).

## 0.6.0

- USB to network bridging.

## 0.5.0

- USB host support for USB MIDI devices.

## 0.4.0

- Web config page, OTA updates and the setup hotspot.

## 0.3.0

- RTP-MIDI session.

## 0.2.0

- WiFi, mDNS and the status LED.

## 0.1.0

- First build.
