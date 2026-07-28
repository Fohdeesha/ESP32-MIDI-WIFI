#include "rtp_midi.h"

#include <Arduino.h>
#include <AppleMIDI.h>
#include <WiFi.h>

#include "config.h"
#include "midi_bridge.h"
#include "status_led.h"

// Defines global session + MIDI interface objects (AppleMIDI, MIDI).
// Must appear in exactly one translation unit -- keep it in this .cpp only.
// The name here is a placeholder; begin() sets the configured name.
APPLEMIDI_CREATE_INSTANCE(WiFiUDP, MIDI, "ESP32-MIDI", 5004);

namespace {
bool started = false;
int s_peerCount = 0;
uint32_t s_lastInviteMs = 0;

// Initiator mode (0.8.0): when a peer target is configured and no session
// is up, invite it. The library retries each invite every 1 s for up to 13
// attempts before cleaning up, so a fresh sendInvite every 30 s never
// stacks pending participants.
constexpr uint32_t INVITE_RETRY_MS = 30000;

void inviteTick() {
    const Config::Values& c = Config::get();
    if (c.targetIp.length() == 0 || s_peerCount > 0) return;
    uint32_t now = millis();
    if (s_lastInviteMs != 0 && now - s_lastInviteMs < INVITE_RETRY_MS) return;
    s_lastInviteMs = now;
    IPAddress ip;
    if (!ip.fromString(c.targetIp.c_str())) {
        Serial.printf("[rtp] invalid peer IP in config: \"%s\"\n", c.targetIp.c_str());
        return;
    }
    if (AppleMIDI.sendInvite(ip, c.targetPort)) {
        Serial.printf("[rtp] inviting peer %s:%u\n", c.targetIp.c_str(), c.targetPort);
    }
}

void onPeerConnected(const APPLEMIDI_NAMESPACE::ssrc_t& /*ssrc*/, const char* name) {
    s_peerCount++;
    Serial.printf("[rtp] peer connected: \"%s\" (peers: %d)\n",
                  (name && name[0]) ? name : "?", s_peerCount);
    StatusLed::set(LedStatus::SessionActive);
}

void onPeerDisconnected(const APPLEMIDI_NAMESPACE::ssrc_t& /*ssrc*/) {
    if (s_peerCount > 0) s_peerCount--;
    Serial.printf("[rtp] peer disconnected (peers: %d)\n", s_peerCount);
    if (s_peerCount == 0) StatusLed::set(LedStatus::WifiConnected);
}

// Incoming MIDI from the RTP peer, handed to the bridge (RTP -> USB).
// These fire inside MIDI.read() in tick(), i.e. loop context.
void onRxNoteOn(byte ch, byte note, byte vel) { MidiBridge::rtpNoteOn(ch, note, vel); }
void onRxNoteOff(byte ch, byte note, byte vel) { MidiBridge::rtpNoteOff(ch, note, vel); }
void onRxControlChange(byte ch, byte num, byte val) { MidiBridge::rtpControlChange(ch, num, val); }
void onRxProgramChange(byte ch, byte num) { MidiBridge::rtpProgramChange(ch, num); }
void onRxAfterTouch(byte ch, byte pressure) { MidiBridge::rtpAfterTouch(ch, pressure); }
void onRxAfterTouchPoly(byte ch, byte note, byte pressure) {
    MidiBridge::rtpAfterTouchPoly(ch, note, pressure);
}
void onRxPitchBend(byte ch, int bend) { MidiBridge::rtpPitchBend(ch, bend); }
void onRxSysEx(byte* data, unsigned size) { MidiBridge::rtpSysEx(data, (uint16_t)size); }
}  // namespace

void RtpMidi::begin() {
    if (started) return;
    AppleMIDI.setName(Config::get().sessionName.c_str());
    AppleMIDI.setHandleConnected(onPeerConnected);
    AppleMIDI.setHandleDisconnected(onPeerDisconnected);
    MIDI.setHandleNoteOn(onRxNoteOn);
    MIDI.setHandleNoteOff(onRxNoteOff);
    MIDI.setHandleControlChange(onRxControlChange);
    MIDI.setHandleProgramChange(onRxProgramChange);
    MIDI.setHandleAfterTouchChannel(onRxAfterTouch);
    MIDI.setHandleAfterTouchPoly(onRxAfterTouchPoly);
    MIDI.setHandlePitchBend(onRxPitchBend);
    MIDI.setHandleSystemExclusive(onRxSysEx);
    MIDI.begin(MIDI_CHANNEL_OMNI);
    started = true;
    Serial.printf("[rtp] session \"%s\" listening on UDP 5004/5005\n",
                  Config::get().sessionName.c_str());
}

bool RtpMidi::isStarted() {
    return started;
}

void RtpMidi::tick() {
    if (!started) return;
    MIDI.read();
    inviteTick();
}

bool RtpMidi::hasPeer() {
    return s_peerCount > 0;
}

int RtpMidi::peerCount() {
    return s_peerCount;
}

void RtpMidi::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (started && s_peerCount > 0) MIDI.sendNoteOn(note, velocity, channel);
}

void RtpMidi::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (started && s_peerCount > 0) MIDI.sendNoteOff(note, velocity, channel);
}

void RtpMidi::sendControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (started && s_peerCount > 0) MIDI.sendControlChange(controller, value, channel);
}

void RtpMidi::sendProgramChange(uint8_t channel, uint8_t program) {
    if (started && s_peerCount > 0) MIDI.sendProgramChange(program, channel);
}

void RtpMidi::sendAfterTouch(uint8_t channel, uint8_t pressure) {
    if (started && s_peerCount > 0) MIDI.sendAfterTouch(pressure, channel);
}

void RtpMidi::sendAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t pressure) {
    if (started && s_peerCount > 0) MIDI.sendAfterTouch(note, pressure, channel);
}

void RtpMidi::sendPitchBend(uint8_t channel, int value) {
    if (started && s_peerCount > 0) MIDI.sendPitchBend(value, channel);
}

void RtpMidi::sendSysEx(const uint8_t* data, uint16_t length) {
    if (started && s_peerCount > 0) MIDI.sendSysEx(length, data, true);
}
