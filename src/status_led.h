#pragma once

enum class LedStatus {
    Boot,            // dim white
    WifiConnecting,  // blinking blue
    WifiConnected,   // solid green
    Error,           // solid red
};

namespace StatusLed {
void begin();
void set(LedStatus status);
void tick();  // call from loop; drives blinking
}  // namespace StatusLed
