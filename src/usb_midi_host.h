#pragma once

#include <WString.h>

#include <cstdint>

namespace UsbMidi {
// Installs the ESP-IDF usb_host stack and starts its event tasks. The OTG
// port (GPIO19/20) becomes host-only from this point -- console must be on
// UART0. Call once from setup().
void begin();
void tick();  // call from loop; drains and logs parsed MIDI events
bool deviceConnected();
const char* deviceName();  // product string of the attached device, "" if none
const char* statusText();  // human-readable host state for the web UI
uint32_t eventCount();     // MIDI events received since boot
// Appends the last few decoded events (oldest first), sep between entries.
void appendRecentEvents(String& out, const char* sep);
// Parsed + raw config descriptor of the last attached device ("" if none).
const char* descriptorDump();
}  // namespace UsbMidi
