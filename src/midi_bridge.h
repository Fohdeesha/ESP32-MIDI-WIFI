#pragma once

#include <cstdint>

namespace MidiBridge {
// Drains parsed USB MIDI packets and forwards virtual cable 0 into the
// RTP-MIDI session (other cables are logged but not bridged). Call from
// loop after RtpMidi::tick().
void tick();
uint32_t forwardedCount();  // USB -> RTP events sent to a peer
uint32_t returnedCount();   // RTP -> USB events queued toward the device

// RTP receive entry points, called from RtpMidi's MIDI callbacks (loop
// context). channel is 1-16; sysex data includes the F0/F7 framing.
void rtpNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void rtpNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
void rtpControlChange(uint8_t channel, uint8_t controller, uint8_t value);
void rtpProgramChange(uint8_t channel, uint8_t program);
void rtpAfterTouch(uint8_t channel, uint8_t pressure);
void rtpAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t pressure);
void rtpPitchBend(uint8_t channel, int value);  // -8192..8191
void rtpSysEx(const uint8_t* data, uint16_t length);
}  // namespace MidiBridge
