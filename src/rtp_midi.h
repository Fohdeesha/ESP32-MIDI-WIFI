#pragma once

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
}  // namespace RtpMidi
