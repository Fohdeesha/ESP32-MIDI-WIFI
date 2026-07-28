#pragma once

#include <cstdint>

namespace MidiBridge {
// Drains parsed USB MIDI packets and forwards virtual cable 0 into the
// RTP-MIDI session (other cables are logged but not bridged). Call from
// loop after RtpMidi::tick().
void tick();
uint32_t forwardedCount();  // events actually sent to an RTP peer
}  // namespace MidiBridge
