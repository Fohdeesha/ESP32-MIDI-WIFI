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
//
// Custom settings instead of APPLEMIDI_CREATE_INSTANCE: the parse buffer must
// hold a whole WiFi datagram. The lathoub rtpMIDI parser keeps state ACROSS
// datagrams (headers-complete flag + command-section countdown over the
// concatenated byte stream), so with the stock 64-byte buffer any datagram
// larger than 64 bytes parses straddling reads -- and one lost datagram
// mid-packet splices the next packet's bytes into the previous command
// section. A garbage length field read that way makes the parser swallow the
// slow trickle of session commands (CK1 answers, invite OKs) for minutes:
// sessions then die with MaxAttempts / NoResponseFromConnectionRequest over
// and over until reboot -- the exact wedge a busy MIDI host's display load
// exposes. Sized so every datagram is parsed whole and the parser state
// returns to idle at each datagram boundary, so a lost packet costs only its
// own contents: 2048 covers the largest unfragmented UDP payload a 1500-byte
// MTU allows (1472) with margin for lwIP handing up a small IP-reassembled
// datagram, which would otherwise straddle the buffer the same way.
struct EspMidiSettings : public APPLEMIDI_NAMESPACE::DefaultSettings {
    static const size_t MaxBufferSize = 2048;
};
using EspMidiSession = APPLEMIDI_NAMESPACE::AppleMIDISession<WiFiUDP, EspMidiSettings>;
EspMidiSession AppleMIDI("ESP32-MIDI", 5004);

// A bridge must be byte-transparent: what arrives on one side has to leave on
// the other unchanged. The MIDI library's DefaultSettings (which AppleMIDI's
// own settings inherit) sets HandleNullVelocityNoteOnAsNoteOff = true, which
// rewrites an incoming Note On with velocity 0 into a Note Off -- a convenience
// for a synth, and wrong for a bridge, because the two are NOT interchangeable
// to every device even though the MIDI spec treats them as equivalent note
// releases.
//
// Measured 2026-07-30 on the Icon P1-M: its LED and touchscreen-cell state is
// driven by Note On velocity 127 (on) / velocity 0 (off) -- the form Icon's own
// DAW scripts send, and the form the surface itself emits for a button release.
// A real Note Off (0x80) is silently IGNORED for that state. So with the
// rewrite in place every host "lamp on" landed and every "lamp off" was dropped
// on the floor: cells and LEDs latched on and could only be cleared by power-
// cycling the surface. The host was sending the correct bytes all along (packet
// capture confirmed 90 4A 00 on the wire) -- this bridge was rewriting them to
// 80 4A 00, and the device-bound event ring showed exactly that ("note off 74").
//
// Turning it off makes rtpNoteOn() fire for a null-velocity Note On, which
// emits USB CIN 0x9 / status 0x90 verbatim. Genuine Note Off messages are
// unaffected -- they still arrive as NoteOff and still go out as 0x80.
struct EspMidiInterfaceSettings : public APPLEMIDI_NAMESPACE::AppleMIDISettings {
    static const bool HandleNullVelocityNoteOnAsNoteOff = false;
};
MIDI_NAMESPACE::MidiInterface<EspMidiSession, EspMidiInterfaceSettings> MIDI(AppleMIDI);

namespace {
bool started = false;
int s_peerCount = 0;
uint32_t s_lastInviteMs = 0;

// Session-event ring for the web UI: connects, disconnects and library
// exceptions with uptime stamps, so session drops are diagnosable on a
// headless device (serial is unplugged while the board sits at the P1-M).
constexpr int EVLOG_SIZE = 24;
char s_evlog[EVLOG_SIZE][48];
int s_evlogNext = 0;

void evlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char* dst = s_evlog[s_evlogNext];
    int n = snprintf(dst, sizeof(s_evlog[0]), "%7lus ", millis() / 1000);
    vsnprintf(dst + n, sizeof(s_evlog[0]) - n, fmt, ap);
    va_end(ap);
    s_evlogNext = (s_evlogNext + 1) % EVLOG_SIZE;
    Serial.printf("[rtp] %s\n", dst);
}

const char* const EXCEPTION_NAMES[] = {
    "BufferFull", "Parse", "UnexpectedParse", "TooManyParticipants",
    "ComputerNotInDirectory", "NotAcceptingAnyone", "UnexpectedInvite",
    "ParticipantNotFound", "ListenerTimeOut", "MaxAttempts",
    "NoResponseFromConnectionRequest", "SendPacketsDropped",
    "ReceivedPacketsDropped", "UdpBeginPacketFailed"};

// Repeat suppression for the event ring (1.5.4). "No bridge is listening yet"
// is this device's EXPECTED idle state, not an exception worth a ring slot: the
// 30 s invite retry wrote TWO entries per attempt (this, plus a phantom
// disconnect), so the 24-entry ring held only ~6 minutes and evicted anything
// genuinely diagnostic -- a real session drop, a USB error -- within minutes of
// it happening. Measured 2026-07-29: 25.7 h of uptime showed four minutes of
// history. Log the first of a run, then one summary per EX_SUMMARY_EVERY.
constexpr uint32_t EX_SUMMARY_EVERY = 20;  // ~10 min at the 30 s retry cadence
int s_lastExCode = -1;
uint32_t s_exRepeat = 0;

void onException(const APPLEMIDI_NAMESPACE::ssrc_t&,
                 const APPLEMIDI_NAMESPACE::Exception& e, const int32_t value) {
    const char* name = (e < sizeof(EXCEPTION_NAMES) / sizeof(EXCEPTION_NAMES[0]))
                           ? EXCEPTION_NAMES[e]
                           : "?";
    if (static_cast<int>(e) == s_lastExCode) {
        if (++s_exRepeat % EX_SUMMARY_EVERY == 0)
            evlog("EX %s x%lu (still retrying)", name,
                  (unsigned long)s_exRepeat);
        return;
    }
    if (s_exRepeat > 0)
        evlog("EX (previous repeated x%lu)", (unsigned long)s_exRepeat);
    s_lastExCode = static_cast<int>(e);
    s_exRepeat = 0;
    evlog("EX %s (%ld)", name, (long)value);
}

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
    evlog("connected \"%s\" (peers %d)", (name && name[0]) ? name : "?", s_peerCount);
    StatusLed::set(LedStatus::SessionActive);
}

void onPeerDisconnected(const APPLEMIDI_NAMESPACE::ssrc_t& /*ssrc*/) {
    // A failed INVITE also lands here when the library cleans up the participant
    // it never established, with the count already 0 (1.5.4). That is not a
    // disconnect: logging it filled the ring every 30 s, and re-blanking the
    // surface each time sent ~110 pointless USB messages. A session that really
    // ended always has a peer to lose first.
    if (s_peerCount == 0) return;
    s_peerCount--;
    evlog("disconnected (peers %d)", s_peerCount);
    if (s_peerCount == 0) {
        StatusLed::set(LedStatus::WifiConnected);
        // An orphaned surface must not keep showing the dead session's last
        // frame as if it were live -- dark is honest (1.2.0).
        MidiBridge::blankSurface();
    }
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
    AppleMIDI.setHandleException(onException);
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
    // Drain every pending message, not one per loop: the AppleMIDI library's
    // available() returns early while its parsed-message buffer is non-empty,
    // skipping socket reads AND initiator clock-sync management on that path.
    // At one read() per loop a busy MIDI host's display/fader stream keeps
    // the buffer full, CK0/CK1 sync starves, and the library ends the session
    // (BY) after MaxSynchronizationCK0Attempts (~60 s). The bound keeps a
    // flood from starving WiFi/web handling in loop().
    for (int i = 0; i < 128 && MIDI.read(); i++) {
    }
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

void RtpMidi::sendActiveSensing() {
    if (started && s_peerCount > 0)
        MIDI.sendRealTime(midi::ActiveSensing);
}

void RtpMidi::appendEventLog(String& out, const char* sep) {
    for (int i = 0; i < EVLOG_SIZE; i++) {
        const char* line = s_evlog[(s_evlogNext + i) % EVLOG_SIZE];
        if (!line[0]) continue;
        out += line;
        out += sep;
    }
}
