#pragma once

#include <cstdint>

namespace RtpMidi {
// Opens the AppleMIDI session listener. WiFi must be connected first.
void begin();
bool isStarted();
void tick();  // call from loop; pumps incoming RTP-MIDI packets
bool hasPeer();
int peerCount();
void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
}  // namespace RtpMidi
