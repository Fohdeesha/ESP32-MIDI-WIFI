#pragma once

#include <WString.h>

#include <cstddef>
#include <cstdint>

// Everything here but hasPeer(), peerCount(), appendEventLog() and
// eventLogVersion() belongs to the MIDI task (see midi_task.h for the
// ownership rules).
namespace RtpMidi {
constexpr int MAX_PEERS = 2;           // the library's participant limit
constexpr size_t PEER_NAME_LEN = 24;   // ...and its session-name limit
// Takes the settings the session needs, copied: the MIDI task never reads the
// live config (Config::save() reassigns its Strings). targetIp "" = accept
// invitations only. Call once from MidiTask::begin(), before the task starts.
void configure(const char* sessionName, const char* targetIp, uint16_t targetPort);
// Opens the AppleMIDI session listener. WiFi must be connected first.
void begin();
bool isStarted();
// Pumps incoming RTP-MIDI packets and the invite cycle. Returns how many
// messages it handled (the MIDI task's timing stats keep the largest).
int tick();
// True when the last tick() stopped on its time or count bound with input
// still waiting (1.9.1): the MIDI task rests a tick before the next one.
bool backlogged();
// A single-writer int: safe to read from any task.
bool hasPeer();
int peerCount();
const char* peerName(int i);  // 0..peerCount()-1
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
// oldest first) for the web status page. Any task: it copies under a lock.
void appendEventLog(String& out, const char* sep);
// Changes whenever appendEventLog()'s output does; 0 while it is empty. Lets
// the web page fetch the log only when there is something new. Any task.
uint32_t eventLogVersion();
}  // namespace RtpMidi
