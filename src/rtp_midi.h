#pragma once

#include <WString.h>

#include <cstdint>

namespace RtpMidi {
// Opens the AppleMIDI session listener. WiFi must be connected first.
void begin();
bool isStarted();
void tick();  // call from loop; pumps incoming RTP-MIDI packets
bool hasPeer();
int peerCount();
// All senders are no-ops until a peer is connected. channel is 1-16.
void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
void sendControlChange(uint8_t channel, uint8_t controller, uint8_t value);
void sendProgramChange(uint8_t channel, uint8_t program);
void sendAfterTouch(uint8_t channel, uint8_t pressure);
void sendAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t pressure);
void sendPitchBend(uint8_t channel, int value);  // -8192..8191
// data must include the F0/F7 framing bytes.
void sendSysEx(const uint8_t* data, uint16_t length);
// System Common: 0xF1 MTC quarter frame and 0xF3 song select (d1), 0xF2 song
// position (d1 = LSB, d2 = MSB), 0xF6 tune request -- data bytes as on the
// wire. Anything else is ignored.
void sendSystemCommon(uint8_t status, uint8_t d1, uint8_t d2);
// System Real-Time: 0xF8 clock, 0xFA start, 0xFB continue, 0xFC stop, 0xFF
// reset. Active Sensing is refused here: toward the host 0xFE is the bridge's
// own heartbeat (sendActiveSensing), which a device's must never stand in for.
void sendRealTime(uint8_t status);
// One MIDI Active Sensing (0xFE) byte -- the session-health heartbeat
// (MidiBridge::healthTick owns the cadence and the device-health gating).
void sendActiveSensing();
// Appends the session-event ring (connects/disconnects/library exceptions,
// oldest first) for the web status page.
void appendEventLog(String& out, const char* sep);
}  // namespace RtpMidi
