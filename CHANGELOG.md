# Changelog

## 1.10.0

- Settings can be exported to a JSON file and imported again, under the new
  "Config import & export" section. The file includes the WiFi and web
  passwords.
- One bad request, no password needed, could hang the web server, and 30 s
  later the watchdog rebooted the board. The core's web server is now patched
  at build time so a request that stalls or trickles is cut off within 20 s.
- A firmware or settings file that can't be used is refused as soon as that is
  clear, instead of after the whole upload.
- A firmware upload sent through another hostname (DNS rebinding) is refused
  before anything is written. Before, the refusal came after the new image
  was already set to boot.
- An invalid peer port is refused instead of silently ignored.
- The page is a little wider, and the status row titles stay on one line and
  stand out more.

## 1.9.1

- A burst of MIDI from the network, like a control surface's full refresh or
  a SysEx dump, could lose its last packets. The network stack only holds 6
  waiting packets, so incoming packets now go straight into a deep queue.
- A long burst no longer holds up MIDI going the other way. It is worked
  through in 5 ms slices; one pass used to take up to 70 ms.
- MIDI from the network is handled as soon as it arrives instead of on the
  next 1 ms poll, and the idle poll is much cheaper.
- `/diag` shows where MIDI time goes per stage, the receive queue
  (`rtp_rx=`), `wakes_net=`, and the receive task's stack.

## 1.9.0

- The web page is now a static file that loads its data as JSON, so the device
  no longer builds the page on every load, and a reload costs almost nothing.
  It looks the same and keeps every setting, row and log.
- Status and any open log update every 2 s while the page is visible.
- New MAC address row on the status page, and `mac=` in `/diag`.
- The network list fills in by itself when a scan finishes.
- The page is UTF-8. A name an older version stored in another encoding is
  left as it is when you save, and a network with such a name can still be
  picked from the list.
- New endpoints: `/api/status`, `/api/config`, `/api/log` and `/api/scan`.

## 1.8.0

- MIDI runs in its own task, above the web server. Loading the page or a slow
  client no longer holds up MIDI, and a firmware upload only pauses it briefly
  while flash is written.
- Serial logging from the MIDI path is queued instead of blocking it.
- Status page shows MIDI task timing and the lowest free heap.
- `/diag` gains `midi_task=`, `loop=`, `log_drops=`, `heap_min=` and
  `stacks=`.

## 1.7.3

- TX power defaults to 8.5 dBm, max 11 dBm. Higher power made WiFi fall apart
  on boards with a weak regulator.
- Saved settings above 11 dBm reset to 8.5 dBm. New 7, 5 and 2 dBm options.
- WiFi reconnects back off (250 ms, doubling up to 4 s) instead of retrying
  instantly.
- Dense MIDI to the network is batched, at most one packet every 10 ms. Single
  events still go out right away.
- Status page and `/diag` show outgoing packet and reconnect counts.

## 1.7.2

- A DAW that disappears without closing its session is dropped after 150 s.
- Long SysEx arrives intact in both directions.
- MIDI clock, transport and timecode are passed through.
- USB errors recover without a replug.
- Short WiFi dropouts no longer open the setup hotspot.
- Power cycling can't roll the firmware back anymore.
- A hung bridge restarts itself.
- Web UI refuses requests from other websites. Open it as esp32-midi.local or
  by IP.

## 1.7.1

- USB devices that fail their first enumeration are retried. A self-powered
  device used to never get picked up again.
- New "USB port" status row and USB host error log.
- `/diag` gains a `usb_port=` line.

## 1.7.0

- WiFi disconnects are logged with the reason code and last RSSI.
- `/diag` shows WiFi stats and the last reset reason (power-on, brownout,
  crash).
- Forced reconnect if WiFi stays down for 15 s.
- TX power is now a setting (default 19.5 dBm, lowered in 1.7.3).

## 1.6.3

- Note On with velocity 0 passes through as-is instead of becoming Note Off.
- Fixes control surface LEDs that turned on but never off.

## 1.6.2

- Resetting the diag counters no longer zeroes the running totals on the status
  page.
- `/diagreset` is POST only.

## 1.6.1

- Reboot button moved next to Save & reboot.

## 1.6.0

- New OUT pipeline stats on the status page: USB transfer latency, queue
  high-water, stalls.
- New plain-text `/diag` endpoint that's cheap to poll, plus `/diagreset`.

## 1.5.4

Never released on its own; shipped in 1.6.0.

- Four USB IN transfers in flight instead of two.
- Session log no longer fills up with invite retries.
- TX power is set before connecting.

## 1.5.3

- Fixed incoming USB MIDI arriving in bunches, then stopping, after transfer
  errors.
- New IN pipeline stats row on the status page.

## 1.5.2

- Reboot button on the config page. Keeps all settings.

## 1.5.1

- Diagnostic logs on the status page are collapsible.
- Bigger log text, red accent, footer.

## 1.5.0

- Fixed MIDI arriving in clumps, and dropped packets, under heavy SysEx traffic.
- The AppleMIDI library printed every SysEx byte to serial. It's patched out at
  build time.
- Viewing the web page no longer starts a WiFi scan during a session.

## 1.4.0

- The bridged USB port can be set separately for each direction.
- Warning when a selected port is past what the device declares.

## 1.3.0

- Pick which USB MIDI interface and port to bridge, or merge all ports.
- Live per-port event counts on the config page.
- Falls back to the first interface if the saved one isn't there.
- Send-only devices work.
- Status page logs MIDI sent to the device too.

## 1.2.0

- Active Sensing every 250 ms while the USB device is healthy.
- Attach/detach SysEx messages (`F0 7D 55 4D 42 01/00 F7`).
- A USB device that stops responding counts as unhealthy.
- Mackie-style surfaces are blanked when the session ends.

## 1.1.0

- Static IP support.

## 1.0.0

- Session no longer starves under heavy inbound traffic.
- Deeper USB send queue.
- Bigger RTP parse buffer, so a lost packet can't corrupt the session.
- Session event log on the status page.

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
