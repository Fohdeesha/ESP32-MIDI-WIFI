#pragma once

#include "config.h"

namespace WifiNet {
// Starts station-mode WiFi and mDNS advertisement. Non-blocking; connection
// state is reported via WiFi events and shown on the status LED. Uses the
// config's WiFi credentials and, when set, its static IP settings (blank
// static IP = DHCP).
void begin(const Config::Values& cfg, const char* hostname);
void tick();          // call from loop
bool isConnected();
bool usingStaticIp();  // true if a valid static IP config was applied
}  // namespace WifiNet
