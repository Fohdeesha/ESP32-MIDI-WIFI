#include "rtp_midi.h"

#include <Arduino.h>
#include <AppleMIDI.h>
#include <WiFi.h>

#include "secrets.h"
#include "status_led.h"

// Defines global session + MIDI interface objects (AppleMIDI, MIDI).
// Must appear in exactly one translation unit — keep it in this .cpp only.
APPLEMIDI_CREATE_INSTANCE(WiFiUDP, MIDI, RTPMIDI_SESSION_NAME, 5004);

namespace {
bool started = false;
int peerCount = 0;

#ifdef RTP_TEST_NOTES
// Short arpeggio sent when a peer connects, so the session can be verified
// end-to-end from the DAW without a USB device attached. Remove once the
// USB bridge (0.5.0) is in place.
constexpr uint8_t ARP_NOTES[] = {60, 64, 67, 72};  // C4 E4 G4 C5
constexpr uint32_t ARP_STEP_MS = 150;
int arpStep = -1;  // -1 = idle; even = note on, odd = note off
uint32_t arpLastMs = 0;

void arpeggioTick() {
    if (arpStep < 0 || peerCount == 0) return;
    uint32_t now = millis();
    if (now - arpLastMs < ARP_STEP_MS) return;
    arpLastMs = now;

    int noteIdx = arpStep / 2;
    if (noteIdx >= (int)sizeof(ARP_NOTES)) {
        arpStep = -1;
        return;
    }
    if (arpStep % 2 == 0) {
        MIDI.sendNoteOn(ARP_NOTES[noteIdx], 100, 1);
    } else {
        MIDI.sendNoteOff(ARP_NOTES[noteIdx], 0, 1);
    }
    arpStep++;
}
#endif

void onPeerConnected(const APPLEMIDI_NAMESPACE::ssrc_t& /*ssrc*/, const char* name) {
    peerCount++;
    Serial.printf("[rtp] peer connected: \"%s\" (peers: %d)\n",
                  (name && name[0]) ? name : "?", peerCount);
    StatusLed::set(LedStatus::SessionActive);
#ifdef RTP_TEST_NOTES
    arpStep = 0;
    arpLastMs = millis();
#endif
}

void onPeerDisconnected(const APPLEMIDI_NAMESPACE::ssrc_t& /*ssrc*/) {
    if (peerCount > 0) peerCount--;
    Serial.printf("[rtp] peer disconnected (peers: %d)\n", peerCount);
    if (peerCount == 0) StatusLed::set(LedStatus::WifiConnected);
}
}  // namespace

void RtpMidi::begin() {
    if (started) return;
    AppleMIDI.setHandleConnected(onPeerConnected);
    AppleMIDI.setHandleDisconnected(onPeerDisconnected);
    MIDI.begin(MIDI_CHANNEL_OMNI);
    started = true;
    Serial.printf("[rtp] session \"%s\" listening on UDP 5004/5005\n", RTPMIDI_SESSION_NAME);
}

bool RtpMidi::isStarted() {
    return started;
}

void RtpMidi::tick() {
    if (!started) return;
    MIDI.read();
#ifdef RTP_TEST_NOTES
    arpeggioTick();
#endif
}

bool RtpMidi::hasPeer() {
    return peerCount > 0;
}

void RtpMidi::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (started && peerCount > 0) MIDI.sendNoteOn(note, velocity, channel);
}

void RtpMidi::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (started && peerCount > 0) MIDI.sendNoteOff(note, velocity, channel);
}
