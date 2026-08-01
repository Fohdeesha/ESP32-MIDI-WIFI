#pragma once

#include "config.h"

namespace WifiNet {
// Starts station-mode WiFi and mDNS advertisement. Non-blocking; connection
// state is reported via WiFi events and shown on the status LED. Uses the
// config's WiFi credentials and, when set, its static IP settings (blank
// static IP = DHCP) and TX power.
void begin(const Config::Values& cfg, const char* hostname);
void tick();          // call from loop
bool isConnected();
bool usingStaticIp();  // true if a valid static IP config was applied

// ── WiFi health diagnostics (1.7.0) ─────────────────────────────────────────
// A dropout used to be invisible: the disconnect handler logged nothing, so a
// device that fell off the network for minutes still read "good RSSI, zero
// errors" once it was back. These expose what actually happened.
uint32_t disconnectCount();   // station disconnect events since boot
uint32_t reconnectKicks();    // forced WiFi.reconnect() calls (see tick)
const char* resetReasonName();  // why the chip last booted (POWERON/BROWNOUT/...)
// One-line machine-readable summary for /diag (no leading/trailing newline).
void appendDiag(String& s);
// Recent WiFi events (connects/disconnects with uptime stamp, reason and last
// known RSSI), newest last, one per line. Empty string when nothing happened.
String eventLog();
}  // namespace WifiNet
