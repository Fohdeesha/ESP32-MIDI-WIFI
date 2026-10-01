#pragma once

#include <cstdint>

namespace BootGuard {
// Call early in setup(), before anything that could crash on a bad image (only
// Serial and LogQueue come first). Counts this boot if the previous run ended in a crash
// (panic or watchdog reset); after 3 such boots without a stable run between
// them, rolls back to the prior OTA slot. Also puts the loop task under the
// task watchdog (30 s), so a hang ends in a counted reset too.
void begin();
// For work that legitimately keeps one loop() pass busy for long -- an OTA
// upload is received inside a single pass: call it per chunk.
void feedWatchdog();
// Call from loop() once per pass; marks the boot stable after a healthy-uptime
// window, and times the pass.
void tick();
// The loop task's longest gap between two tick() calls since boot or the last
// resetLoopStats(). Since 1.8.0 a slow page load or OTA upload shows up here,
// and no longer in the MIDI task's timing. Loop task only.
uint32_t loopPeriodMaxUs();
void resetLoopStats();
// Call before any intentional ESP.restart() so clean reboots never count
// toward the crash-loop threshold.
void markStable();
}  // namespace BootGuard
