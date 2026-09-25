#pragma once

namespace BootGuard {
// Call FIRST in setup(). Counts this boot if the previous run ended in a crash
// (panic or watchdog reset); after 3 such boots without a stable run between
// them, rolls back to the prior OTA slot. Also puts the loop task under the
// task watchdog (30 s), so a hang ends in a counted reset too.
void begin();
// For work that legitimately keeps one loop() pass busy for long -- an OTA
// upload is received inside a single pass: call it per chunk.
void feedWatchdog();
// Call from loop(); marks the boot stable after a healthy-uptime window.
void tick();
// Call before any intentional ESP.restart() so clean reboots never count
// toward the crash-loop threshold.
void markStable();
}  // namespace BootGuard
