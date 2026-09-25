#include "rtp_midi.h"

#include <Arduino.h>
#include <AppleMIDI.h>
#include <WiFi.h>

#include "config.h"
#include "midi_bridge.h"

// tools/patch_applemidi.py fixes a set of AppleMIDI 3.3.0 bugs at build time
// (a session the DAW opened never timing out, a lost OK locking a peer out,
// journal and datagram parsing -- see the script). Never build against a copy
// it has not patched, or has patched with an older set.
#if !defined(ESP32_MIDI_WIFI_APPLEMIDI_PATCHSET) || ESP32_MIDI_WIFI_APPLEMIDI_PATCHSET != 2
#error "AppleMIDI is unpatched or stale: delete .pio/libdeps and rebuild (tools/patch_applemidi.py)"
#endif

// Defines global session + MIDI interface objects (AppleMIDI, MIDI).
// Must appear in exactly one translation unit -- keep it in this .cpp only.
// The name here is a placeholder; begin() sets the configured name.
//
// Custom settings instead of APPLEMIDI_CREATE_INSTANCE.
//
// MaxBufferSize: the parse buffer must hold a whole WiFi datagram. With the
// stock 64 bytes a datagram was parsed straddling reads, and the parser kept
// its state across datagrams, so one lost or truncated datagram spliced the
// next packet's bytes into its command section. A garbage length field read
// that way swallowed the slow trickle of session commands (CK1 answers, invite
// OKs) for minutes: sessions died with MaxAttempts /
// NoResponseFromConnectionRequest over and over until reboot -- the exact
// wedge a busy MIDI host's display load exposed. WiFiUDP hands over at most
// 1460 bytes of a datagram (it truncates anything longer), so 2048 always
// holds one whole, and the patched library parses each datagram on its own
// and drops whatever it leaves (1.7.2): a bad or truncated datagram costs only
// its own contents.
//
// CK_MaxTimeOut: a session the DAW opened is ended (BY sent, surface blanked)
// after this long without a clock-sync exchange from it. The protocol has the
// initiator sync at least once every 60 s; 150 s rides out a lost sync even
// at that slowest legal cadence. (Until 1.7.2 this build never applied the
// timeout at all -- see the listener-timeout patch -- so a DAW that vanished
// without a BY stayed "connected" forever.)
struct EspMidiSettings : public APPLEMIDI_NAMESPACE::DefaultSettings {
    static const size_t MaxBufferSize = 2048;
    static const unsigned long CK_MaxTimeOut = 150000;
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
uint32_t s_lastInviteMs = 0;

// Connected sessions, by the peer's SSRC (1.7.2). The library's callbacks do
// not map one-to-one onto sessions: "connected" fires again for a
// retransmitted data-port IN or a late duplicate OK, and "disconnected" also
// fires for an invitation that never connected or a half-open session timing
// out. The plain counter this replaces drifted on each of those -- left at 1
// after the last session ended (no blanking, and inviteTick() never inviting
// again) or dropped to 0 under a live one (surface blanked, USB->RTP muted).
// Keyed by identity, every callback is idempotent.
constexpr int MAX_PEERS = EspMidiSettings::MaxNumberOfParticipants;
constexpr size_t PEER_NAME_LEN = EspMidiSettings::MaxSessionNameLen;
APPLEMIDI_NAMESPACE::ssrc_t s_peerSsrc[MAX_PEERS];
char s_peerName[MAX_PEERS][PEER_NAME_LEN + 1];
int s_peerCount = 0;

int findPeer(APPLEMIDI_NAMESPACE::ssrc_t ssrc) {
    for (int i = 0; i < s_peerCount; i++)
        if (s_peerSsrc[i] == ssrc) return i;
    return -1;
}

// Session-event ring for the web UI: connects, disconnects and library
// exceptions with uptime stamps, so session drops are diagnosable on a
// headless device (serial is unplugged while the board sits at the P1-M).
constexpr int EVLOG_SIZE = 24;
char s_evlog[EVLOG_SIZE][64];
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

constexpr size_t EX_KINDS = sizeof(EXCEPTION_NAMES) / sizeof(EXCEPTION_NAMES[0]);

// Rate limit for the event ring, per exception kind: the first occurrence is
// logged, then at most one line per EX_LOG_GAP_MS, carrying the number held
// back since the previous line; exact totals follow the ring on the page.
// "No bridge is listening yet" is this device's EXPECTED idle state in
// initiator mode, not something worth a slot every 30 s: that evicted anything
// genuinely diagnostic within minutes (1.5.4 -- measured, 25.7 h of uptime
// showed four minutes of history). Bursts are the other hazard (1.7.2): one
// junk datagram used to raise an exception per byte, and the old count-based
// summary (a line per 20) turned that into dozens of lines that rewrote the
// whole ring at once.
constexpr uint32_t EX_LOG_GAP_MS = 10 * 60 * 1000;
struct ExLog {
    bool seen;
    uint32_t lastMs;
    uint32_t heldBack;
    uint32_t total;
};
ExLog s_exLog[EX_KINDS + 1];  // + one slot for codes this build has no name for

void onException(const APPLEMIDI_NAMESPACE::ssrc_t&,
                 const APPLEMIDI_NAMESPACE::Exception& e, const int32_t value) {
    const size_t kind = static_cast<size_t>(e) < EX_KINDS ? static_cast<size_t>(e) : EX_KINDS;
    const char* name = kind < EX_KINDS ? EXCEPTION_NAMES[kind] : "?";
    ExLog& ex = s_exLog[kind];
    ex.total++;
    const uint32_t now = millis();
    if (ex.seen && now - ex.lastMs < EX_LOG_GAP_MS) {
        ex.heldBack++;
        return;
    }
    if (ex.heldBack > 0)
        evlog("EX %s (%ld) +%lu more", name, (long)value, (unsigned long)ex.heldBack);
    else
        evlog("EX %s (%ld)", name, (long)value);
    ex.seen = true;
    ex.lastMs = now;
    ex.heldBack = 0;
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

void onPeerConnected(const APPLEMIDI_NAMESPACE::ssrc_t& ssrc, const char* name) {
    int i = findPeer(ssrc);
    if (i >= 0) {
        // The peer re-sent its IN or OK: the session was already up.
        evlog("re-connected \"%s\" (peers %d)", s_peerName[i], s_peerCount);
        return;
    }
    if (s_peerCount == MAX_PEERS) return;  // cannot happen: the library caps it
    i = s_peerCount++;
    s_peerSsrc[i] = ssrc;
    // The library's copy may lack its terminator (fixed by the name-nul
    // patch); never read past the field either way.
    snprintf(s_peerName[i], sizeof(s_peerName[i]), "%.*s", (int)PEER_NAME_LEN,
             (name && name[0]) ? name : "?");
    evlog("connected \"%s\" (peers %d)", s_peerName[i], s_peerCount);
}

void onPeerDisconnected(const APPLEMIDI_NAMESPACE::ssrc_t& ssrc) {
    // Also fired for sessions that never connected -- a failed invitation, a
    // half-open one timing out. Not a disconnect: logging those filled the
    // ring every 30 s, and re-blanking the surface each time sent ~110
    // pointless USB messages (1.5.4).
    const int i = findPeer(ssrc);
    if (i < 0) return;
    evlog("disconnected \"%s\" (peers %d)", s_peerName[i], s_peerCount - 1);
    s_peerCount--;
    if (i != s_peerCount) {  // move the last entry into the gap
        s_peerSsrc[i] = s_peerSsrc[s_peerCount];
        memcpy(s_peerName[i], s_peerName[s_peerCount], sizeof(s_peerName[i]));
    }
    if (s_peerCount == 0) {
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
// System Common and Real-Time (1.7.2): MIDI clock, transport and timecode.
void onRxTimeCodeQuarterFrame(byte data) { MidiBridge::rtpSystemCommon(0xF1, data, 0); }
void onRxSongPosition(unsigned beats) {
    MidiBridge::rtpSystemCommon(0xF2, beats & 0x7F, (beats >> 7) & 0x7F);
}
void onRxSongSelect(byte song) { MidiBridge::rtpSystemCommon(0xF3, song, 0); }
void onRxTuneRequest() { MidiBridge::rtpSystemCommon(0xF6, 0, 0); }
void onRxClock() { MidiBridge::rtpRealTime(0xF8); }
void onRxStart() { MidiBridge::rtpRealTime(0xFA); }
void onRxContinue() { MidiBridge::rtpRealTime(0xFB); }
void onRxStop() { MidiBridge::rtpRealTime(0xFC); }
void onRxActiveSensing() { MidiBridge::rtpRealTime(0xFE); }
void onRxSystemReset() { MidiBridge::rtpRealTime(0xFF); }
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
    MIDI.setHandleTimeCodeQuarterFrame(onRxTimeCodeQuarterFrame);
    MIDI.setHandleSongPosition(onRxSongPosition);
    MIDI.setHandleSongSelect(onRxSongSelect);
    MIDI.setHandleTuneRequest(onRxTuneRequest);
    MIDI.setHandleClock(onRxClock);
    MIDI.setHandleStart(onRxStart);
    MIDI.setHandleContinue(onRxContinue);
    MIDI.setHandleStop(onRxStop);
    MIDI.setHandleActiveSensing(onRxActiveSensing);
    MIDI.setHandleSystemReset(onRxSystemReset);
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
    //
    // read() returning false is NOT "nothing left" (1.7.2): it is also what
    // the MIDI library returns after handing over each 128-byte piece of a
    // longer SysEx (and after a parse error). Stopping there spread a 1.4 kB
    // display frame over 12 loop passes, with the library's socket reads and
    // session upkeep skipped for all of them. Stop when the session has no
    // buffered input instead; available() is exactly what read() would call
    // next, so asking it costs nothing a further read wouldn't.
    for (int i = 0; i < 128; i++) {
        if (!MIDI.read() && AppleMIDI.available() == 0) break;
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

void RtpMidi::sendSystemCommon(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!started || s_peerCount == 0) return;
    switch (status) {
        case 0xF1: MIDI.sendTimeCodeQuarterFrame(d1 & 0x7F); break;
        case 0xF2: MIDI.sendSongPosition((d1 & 0x7F) | ((d2 & 0x7F) << 7)); break;
        case 0xF3: MIDI.sendSongSelect(d1 & 0x7F); break;
        case 0xF6: MIDI.sendTuneRequest(); break;
        default: break;
    }
}

void RtpMidi::sendRealTime(uint8_t status) {
    // 0xFE toward the host is the bridge's own heartbeat only (see the header).
    if (!started || s_peerCount == 0 || status == 0xFE) return;
    // The library sends F8, FA, FB, FC and FF and ignores anything else.
    MIDI.sendRealTime(static_cast<midi::MidiType>(status));
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
    // Exact per-kind totals since boot, which the ring's rate limit hides.
    bool any = false;
    for (size_t k = 0; k <= EX_KINDS; k++) {
        if (s_exLog[k].total == 0) continue;
        out += any ? ", " : "exceptions since boot: ";
        out += k < EX_KINDS ? EXCEPTION_NAMES[k] : "?";
        out += " x";
        out += String((unsigned long)s_exLog[k].total);
        any = true;
    }
    if (any) out += sep;
}
