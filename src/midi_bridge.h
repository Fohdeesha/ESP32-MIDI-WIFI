#pragma once

#include <cstdint>

namespace MidiBridge {
// Drains parsed USB MIDI packets and forwards virtual cable 0 into the
// RTP-MIDI session (other cables are logged but not bridged). Call from
// loop after RtpMidi::tick().
void tick();
// Session-health tick (1.2.0), call from loop after tick():
//   * While a peer is connected AND the USB device is healthy
//     (UsbMidi::healthy() -- attached, IN pipeline live, OUT ACKs flowing),
//     sends MIDI Active Sensing (0xFE) every 250 ms. Hosts can treat its
//     absence as "the bridge or its device is gone" within ~1 s, per the
//     0xFE contract, instead of waiting out a session timeout. The heartbeat
//     is deliberately derived from device-side evidence only -- this bridge
//     never asserts liveness it cannot see.
//   * On a device attach/detach edge (and once at each peer connect), sends a
//     one-shot status marker sysex F0 7D 55 4D 42 <state> F7 (0x7D = the MIDI
//     educational/private manufacturer ID, "UMB" tag; state 01 = attached,
//     00 = detached) so a host learns of an unplug immediately.
void healthTick();
// Clears a Mackie-Control-family surface (displays blank, LEDs/rings/meters
// dark) straight over USB. Called when the last RTP peer disconnects: a
// frozen display full of live-looking values on an orphaned surface is worse
// than a dark one. Unknown sysex is ignored by non-MCU devices, so this is
// safe to send to anything.
void blankSurface();
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
