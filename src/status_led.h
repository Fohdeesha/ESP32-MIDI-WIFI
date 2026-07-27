#pragma once

enum class LedStatus {
    Boot,            // dim white
    WifiConnecting,  // blinking blue
    WifiConnected,   // solid green
    SessionActive,   // solid cyan — RTP-MIDI peer connected
    PortalActive,    // blinking magenta — setup AP / config portal
    Error,           // solid red
};

namespace StatusLed {
void begin();
void set(LedStatus status);
void tick();  // call from loop; drives blinking
}  // namespace StatusLed
