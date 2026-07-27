#pragma once

namespace WifiNet {
// Starts station-mode WiFi and mDNS advertisement. Non-blocking; connection
// state is reported via WiFi events and shown on the status LED.
void begin(const char* ssid, const char* password, const char* hostname);
void tick();  // call from loop
bool isConnected();
}  // namespace WifiNet
