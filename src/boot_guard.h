#pragma once

namespace BootGuard {
// Call FIRST in setup(). Counts this boot attempt; if the previous firmware
// crash-looped (3 failed boots), rolls back to the prior OTA slot.
void begin();
// Call from loop(); marks the boot stable after a healthy-uptime window.
void tick();
// Call before any intentional ESP.restart() so clean reboots never count
// toward the crash-loop threshold.
void markStable();
}  // namespace BootGuard
