#include "midi_bridge.h"

#include <Arduino.h>

#include <cstring>

#include "config.h"
#include "rtp_midi.h"
#include "usb_midi_host.h"

// USB -> network direction first, RTP -> USB below.
namespace {

// Which virtual cable of the claimed USB MIDI function is bridged. A device
// can present up to 16 cables (the "ports" a DAW lists) on one interface;
// RTP-MIDI has no cable concept, so exactly one is bridged unless the user
// asks for CABLE_ALL and accepts that they arrive merged -- including, while
// one cable's SysEx is long enough to go out in segments, other cables'
// messages between those segments, which RFC 6295 does not allow by default
// (a strict receiver may drop that SysEx).
uint8_t s_cable = 0;
// Cable for the network->device direction, resolved from config, and the same
// value pre-shifted into the high nibble stamped on outgoing packets.
uint8_t s_outCable = 0;
uint8_t s_outNibble = 0;

uint32_t s_forwarded = 0;

// SysEx arrives as 3-byte chunks (CIN 0x4) closed by a 1/2/3-byte end chunk
// (CIN 0x5-0x7) and is collected here: a message that fits goes out whole,
// exactly as before. Until 1.7.2 one that did not fit (then 256 bytes) was
// dropped outright -- a synth's patch dump, say, never arrived. Now it leaves
// as RFC 6295 segments (F0..F0, F7..F0, ..., F7..F7), the same form AppleMIDI
// itself uses for SysEx that outgrows its buffer, which every RTP-MIDI
// receiver reassembles.
constexpr uint16_t SYSEX_SEG = 512;  // bytes per segment, markers included
uint8_t s_sysex[SYSEX_SEG + 1];      // + the trailing "more follows" F0
uint16_t s_sysexLen = 0;             // 0 = no SysEx in progress
bool s_sysexSegmented = false;       // part of this message is already sent
uint8_t s_sysexCable = 0;            // cable the message in progress is on

void sysexByte(uint8_t b) {
    if (s_sysexLen == SYSEX_SEG) {
        s_sysex[s_sysexLen++] = 0xF0;  // "continues" marker
        RtpMidi::sendSysEx(s_sysex, s_sysexLen);
        s_sysexSegmented = true;
        s_sysex[0] = 0xF7;  // the next segment opens with the continuation marker
        s_sysexLen = 1;
    }
    s_sysex[s_sysexLen++] = b;
}

// Drops the message in progress. Part of it may already be on the wire: the
// RFC 6295 cancel sublist (F7 F4) tells the receiver to discard that part, so
// its reassembly is neither left waiting for a tail that will not come nor
// handed the truncated message as if it were complete.
void sysexAbandon() {
    if (s_sysexSegmented) {
        static const uint8_t CANCEL[2] = {0xF7, 0xF4};
        RtpMidi::sendSysEx(CANCEL, sizeof(CANCEL));
    }
    s_sysexLen = 0;
    s_sysexSegmented = false;
}

// One USB-MIDI SysEx packet: CIN 0x4 = 3 bytes that start or continue the
// message, CIN 0x5-0x7 = 1-3 bytes that end it.
void sysexPacket(const uint8_t p[4]) {
    const uint8_t cin = p[0] & 0x0F;
    const uint8_t cable = p[0] >> 4;
    if (p[1] == 0xF0) {
        sysexAbandon();  // a new message ends one that never saw its F7
        s_sysexCable = cable;
    } else if (!s_sysexLen || cable != s_sysexCable) {
        // Not part of a message in progress: its start was missed, or it is
        // another cable's SysEx interleaved into a merged stream -- which
        // cannot be spliced into this one either way.
        return;
    }
    const uint8_t n = cin == 0x4 ? 3 : cin - 0x4;
    for (uint8_t i = 0; i < n; i++) sysexByte(p[1 + i]);
    if (cin == 0x4) return;
    // An end packet whose last byte is not EOX is out of spec; terminate the
    // message anyway rather than leave the receiver collecting forever.
    if (p[n] != 0xF7) sysexByte(0xF7);
    RtpMidi::sendSysEx(s_sysex, s_sysexLen);
    s_forwarded++;
    s_sysexLen = 0;
    s_sysexSegmented = false;
}

// At most this many USB packets per tick(). Everything tick() queues toward
// the peer between two RtpMidi::tick() calls leaves as ONE RTP datagram, and
// WiFiUDP splits anything over 1460 bytes into a truncated packet plus a
// headerless remainder that no receiver can parse. Per packet at most 5 bytes
// are queued (3 data bytes, an EOX the device left out, the delta-time byte),
// plus the SysEx collected over earlier ticks (at most one segment): bounded
// here however fast the USB client task refills the queue while it drains.
constexpr int USB_BATCH = 128;
static_assert(SYSEX_SEG + 1 + USB_BATCH * 5 + 64 <= 1400,
              "one loop's USB->RTP output must fit one WiFiUDP datagram");

bool s_tickPeer = false;  // RtpMidi::hasPeer() as the last tick() saw it

}  // namespace

void MidiBridge::begin(uint8_t cable, uint8_t outCable) {
    s_cable = (cable == Config::CABLE_ALL || cable <= 15) ? cable : 0;
    // Anything not an explicit cable means "follow the input". Merging every
    // input cable leaves no single one to follow, so that resolves to cable 0
    // -- the cable every compliant device implements.
    s_outCable = outCable <= 15 ? outCable
                                : (s_cable == Config::CABLE_ALL ? 0 : s_cable);
    s_outNibble = (uint8_t)(s_outCable << 4);
    if (s_cable == Config::CABLE_ALL) {
        Serial.printf("[bridge] bridging all virtual cables in (merged), cable %u out\n",
                      s_outCable);
    } else {
        Serial.printf("[bridge] bridging virtual cable %u in, cable %u out\n", s_cable,
                      s_outCable);
    }
}

uint8_t MidiBridge::bridgedCable() {
    return s_cable;
}

uint8_t MidiBridge::outputCable() {
    return s_outCable;
}

void MidiBridge::tick() {
    // A session starting or ending: a message collected for the previous one
    // (or for none) must not be glued onto the new one's traffic.
    const bool peer = RtpMidi::hasPeer();
    if (peer != s_tickPeer) {
        s_tickPeer = peer;
        s_sysexLen = 0;
        s_sysexSegmented = false;
    }
    uint8_t p[4];
    for (int n = 0; n < USB_BATCH && UsbMidi::readPacket(p); n++) {
        if (s_cable != Config::CABLE_ALL && (p[0] >> 4) != s_cable) continue;
        if (!peer) continue;  // nobody to send to
        uint8_t ch = (p[1] & 0x0F) + 1;
        switch (p[0] & 0x0F) {
            case 0x8:
                RtpMidi::sendNoteOff(ch, p[2], p[3]);
                s_forwarded++;
                break;
            case 0x9:
                RtpMidi::sendNoteOn(ch, p[2], p[3]);
                s_forwarded++;
                break;
            case 0xA:
                RtpMidi::sendAfterTouchPoly(ch, p[2], p[3]);
                s_forwarded++;
                break;
            case 0xB:
                RtpMidi::sendControlChange(ch, p[2], p[3]);
                s_forwarded++;
                break;
            case 0xC:
                RtpMidi::sendProgramChange(ch, p[2]);
                s_forwarded++;
                break;
            case 0xD:
                RtpMidi::sendAfterTouch(ch, p[2]);
                s_forwarded++;
                break;
            case 0xE:
                RtpMidi::sendPitchBend(ch, (p[2] | (p[3] << 7)) - 8192);
                s_forwarded++;
                break;
            case 0x4:  // SysEx starts or continues (3 bytes)
            case 0x6:  // SysEx ends with 2 bytes
            case 0x7:  // SysEx ends with 3 bytes
                sysexPacket(p);
                break;
            case 0x5:  // SysEx ends with 1 byte -- or a 1-byte System Common
                if (p[1] == 0xF7) {
                    sysexPacket(p);
                } else if (p[1] == 0xF6) {  // tune request
                    RtpMidi::sendSystemCommon(0xF6, 0, 0);
                    s_forwarded++;
                }
                break;
            // System Common and Real-Time (1.7.2): MIDI clock, transport and
            // timecode, which were dropped until then.
            case 0x2:  // 2 bytes: MTC quarter frame (F1), song select (F3)
            case 0x3:  // 3 bytes: song position (F2)
                if (p[1] == 0xF1 || p[1] == 0xF2 || p[1] == 0xF3) {
                    RtpMidi::sendSystemCommon(p[1], p[2], p[3]);
                    s_forwarded++;
                }
                break;
            case 0xF:  // 1 byte: real-time (a raw data byte is not parseable alone)
                // Not Active Sensing: toward the host, 0xFE is the bridge's own
                // heartbeat, gated on device health (healthTick).
                if (p[1] == 0xF8 || p[1] == 0xFA || p[1] == 0xFB || p[1] == 0xFC ||
                    p[1] == 0xFF) {
                    RtpMidi::sendRealTime(p[1]);
                    s_forwarded++;
                }
                break;
            default:  // CIN 0x0/0x1: reserved
                break;
        }
    }
}

uint32_t MidiBridge::forwardedCount() {
    return s_forwarded;
}

// ---- RTP -> USB (0.7.0) ----------------------------------------------------
// Everything goes out on the configured cable, mirroring the inbound filter.

namespace {
uint32_t s_returned = 0;

// SysEx toward the device arrives in SEGMENTS whenever it is long: the MIDI
// library splits anything over its SysExMaxSize (128 bytes), and AppleMIDI
// passes on the segmentation RFC 6295 itself uses for SysEx spanning RTP
// packets (which macOS applies to large SysEx). Both mark the pieces the same
// way -- first F0..F0, middle F7..F0, last F7..F7 (whole message: F0..F7) --
// and those inner F0/F7 are markers, not data. Until 1.7.2 each piece went to
// the device as if it were a complete message, so every SysEx over 128 bytes
// was cut short (a CIN 0x5-0x7 end packet carrying a bogus F0) and followed by
// a second "SysEx" starting with a bogus F7: display frames above 128 bytes
// reached the surface torn. Segments are now stitched back into ONE USB-MIDI
// SysEx: markers dropped, and up to two bytes carried across each boundary so
// every packet but the last is a full CIN 0x4 triple.
uint8_t s_sxPend[3];      // bytes waiting to fill a 3-byte SysEx packet
uint8_t s_sxPendLen = 0;
bool s_sxOpen = false;    // a started SysEx is still waiting for its F7
bool s_sxOk = true;       // every packet of the current SysEx was queued

void sxByte(uint8_t b) {
    s_sxPend[s_sxPendLen++] = b;
    if (s_sxPendLen == 3) {
        uint8_t pkt[4] = {(uint8_t)(s_outNibble | 0x4), s_sxPend[0], s_sxPend[1], s_sxPend[2]};
        s_sxOk &= UsbMidi::writePacket(pkt);
        s_sxPendLen = 0;
    }
}

// Terminates the open SysEx: what is pending plus the F7 go out as the
// CIN 0x5/0x6/0x7 end packet (1-3 bytes).
void sxEnd() {
    s_sxPend[s_sxPendLen++] = 0xF7;
    uint8_t pkt[4] = {(uint8_t)(s_outNibble | (0x4 + s_sxPendLen)), 0, 0, 0};
    memcpy(&pkt[1], s_sxPend, s_sxPendLen);
    s_sxOk &= UsbMidi::writePacket(pkt);
    s_sxPendLen = 0;
    s_sxOpen = false;
}

// Every message but SysEx and real-time.
void usbSend(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2) {
    // A data byte with its top bit set is a status byte where data belongs --
    // a malformed message, which the MIDI library passes on as-is (it only
    // resynchronises on real-time and EOX). No device is sent one.
    if ((d1 | d2) & 0x80) return;
    // Any status but real-time ends a SysEx, and RFC 6295 allows nothing else
    // between SysEx segments: close one still open, so the device's parser is
    // back at a message boundary and a late continuation is dropped rather
    // than taken for this message's data.
    if (s_sxOpen) sxEnd();
    uint8_t pkt[4] = {(uint8_t)(s_outNibble | cin), status, d1, d2};
    if (UsbMidi::writePacket(pkt)) s_returned++;
}
}  // namespace

void MidiBridge::rtpNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    usbSend(0x9, 0x90 | (ch - 1), note, vel);
}

void MidiBridge::rtpNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
    usbSend(0x8, 0x80 | (ch - 1), note, vel);
}

void MidiBridge::rtpControlChange(uint8_t ch, uint8_t controller, uint8_t value) {
    usbSend(0xB, 0xB0 | (ch - 1), controller, value);
}

void MidiBridge::rtpProgramChange(uint8_t ch, uint8_t program) {
    usbSend(0xC, 0xC0 | (ch - 1), program, 0);
}

void MidiBridge::rtpAfterTouch(uint8_t ch, uint8_t pressure) {
    usbSend(0xD, 0xD0 | (ch - 1), pressure, 0);
}

void MidiBridge::rtpAfterTouchPoly(uint8_t ch, uint8_t note, uint8_t pressure) {
    usbSend(0xA, 0xA0 | (ch - 1), note, pressure);
}

void MidiBridge::rtpPitchBend(uint8_t ch, int value) {
    uint16_t v = (uint16_t)(value + 8192);
    usbSend(0xE, 0xE0 | (ch - 1), v & 0x7F, (v >> 7) & 0x7F);
}

void MidiBridge::rtpSysEx(const uint8_t* data, uint16_t length) {
    if (length < 2) return;
    const bool starts = data[0] == 0xF0;  // a whole message, or its first segment
    if (!starts && data[0] != 0xF7) return;  // not SysEx framing at all
    const uint8_t last = data[length - 1];
    const bool ends = last == 0xF7;       // a whole message, or its last segment
    if (starts) {
        // A start while one is still open means that one's tail was lost
        // (a dropped RTP packet). Close it where it stands so the device's
        // parser is never left splicing two messages together.
        if (s_sxOpen) sxEnd();
        s_sxOpen = true;
        s_sxOk = true;
        s_sxPendLen = 0;
        sxByte(0xF0);
    } else if (!s_sxOpen) {
        return;  // a continuation whose start never arrived: nothing to attach to
    }
    // The body lies between the leading byte (F0, or the F7 marker) and the
    // trailing F7 / F0 marker. A trailing byte that is neither is data.
    const uint16_t bodyEnd = (last == 0xF7 || last == 0xF0) ? length - 1 : length;
    for (uint16_t i = 1; i < bodyEnd; i++) {
        if (data[i] & 0x80) {
            // A status byte inside SysEx data: malformed (or an RFC 6295
            // cancel, F7 F4). It ends the message, as it would on a MIDI cable.
            sxEnd();
            return;
        }
        sxByte(data[i]);
    }
    if (ends) {
        sxEnd();
        if (s_sxOk) s_returned++;  // one event per complete SysEx
    }
}

void MidiBridge::rtpSystemCommon(uint8_t status, uint8_t d1, uint8_t d2) {
    // USB-MIDI CIN 0x2 = two-byte (F1, F3), 0x3 = three-byte (F2), 0x5 =
    // single-byte System Common (F6).
    const uint8_t cin = status == 0xF2 ? 0x3 : status == 0xF6 ? 0x5 : 0x2;
    usbSend(cin, status, d1, d2);
}

void MidiBridge::rtpRealTime(uint8_t status) {
    // Real-time may sit inside a SysEx without ending it, so it bypasses
    // usbSend() and leaves an open SysEx alone.
    uint8_t pkt[4] = {(uint8_t)(s_outNibble | 0xF), status, 0, 0};
    if (UsbMidi::writePacket(pkt)) s_returned++;
}

uint32_t MidiBridge::returnedCount() {
    return s_returned;
}

// ---- Session health (1.2.0) ------------------------------------------------

namespace {
constexpr uint32_t HEARTBEAT_MS = 250;
uint32_t s_lastBeatMs = 0;
bool s_lastDevState = false;
bool s_lastPeerState = false;

void sendDeviceMarker(bool attached) {
    // F0 7D "UMB" <state> F7 -- see the healthTick() contract in the header.
    const uint8_t marker[7] = {0xF0, 0x7D, 0x55, 0x4D,
                               0x42, attached ? (uint8_t)0x01 : (uint8_t)0x00,
                               0xF7};
    RtpMidi::sendSysEx(marker, sizeof(marker));
    Serial.printf("[health] device %s marker sent\n",
                  attached ? "attached" : "detached");
}
}  // namespace

void MidiBridge::healthTick() {
    const bool peer = RtpMidi::hasPeer();
    const bool dev = UsbMidi::deviceConnected();

    // A device that comes or goes mid-SysEx must not have the rest of that
    // message delivered to whatever is attached next, in either direction:
    // toward the device, its closing F7 is simply never sent; toward the host,
    // a part already on the wire is cancelled -- before the marker below,
    // which would otherwise land inside the open SysEx (1.7.2).
    if (dev != s_lastDevState) {
        s_sxOpen = false;
        s_sxPendLen = 0;
        sysexAbandon();
    }
    // Attach/detach edges reach the peer immediately; a fresh peer gets the
    // current state once so it never has to guess.
    if (peer && (dev != s_lastDevState || !s_lastPeerState)) sendDeviceMarker(dev);
    s_lastDevState = dev;
    s_lastPeerState = peer;

    if (!peer) return;
    const uint32_t now = millis();
    if (now - s_lastBeatMs < HEARTBEAT_MS) return;
    // The heartbeat is GATED on device health, not merely on attachment: it
    // must stop the moment the bridge can no longer vouch for the device
    // (IN pipeline dead, OUT transfers unACKed), so the host's watchdog trips.
    if (UsbMidi::healthy()) {
        s_lastBeatMs = now;
        RtpMidi::sendActiveSensing();
    }
}

// ---- Surface blanking on session loss (1.2.0) ------------------------------

namespace {
// Chunk one complete sysex (F0..F7) into USB-MIDI event packets on the
// configured cable.
void usbSysEx(const uint8_t* data, uint16_t length) {
    uint16_t i = 0;
    while (length - i > 3) {
        uint8_t pkt[4] = {(uint8_t)(s_outNibble | 0x4), data[i], data[i + 1], data[i + 2]};
        UsbMidi::writePacket(pkt);
        i += 3;
    }
    uint8_t rem = length - i;
    uint8_t pkt[4] = {(uint8_t)(s_outNibble | (0x4 + rem)), 0, 0, 0};
    memcpy(&pkt[1], &data[i], rem);
    UsbMidi::writePacket(pkt);
}

void usbShort(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t pkt[4] = {(uint8_t)(s_outNibble | cin), status, d1, d2};
    UsbMidi::writePacket(pkt);
}

// Blank one 56-char half of an MCU-family scribble display: header + position
// + spaces + F7.
void blankHalf(const uint8_t* hdr6, uint8_t pos) {
    uint8_t frame[6 + 1 + 56 + 1];
    memcpy(frame, hdr6, 6);
    frame[6] = pos;
    memset(frame + 7, ' ', 56);
    frame[sizeof(frame) - 1] = 0xF7;
    usbSysEx(frame, sizeof(frame));
}
}  // namespace

void MidiBridge::blankSurface() {
    // A SysEx the dead session left half-sent can never be finished: close it
    // first, so the device's parser is back at a message boundary.
    if (s_sxOpen) sxEnd();
    if (!UsbMidi::deviceConnected()) return;
    Serial.println("[health] session gone -- blanking the surface");
    // Both scribble families an Icon P1-M carries: the classic MCU header and
    // Icon's extended second pair. Harmless elsewhere (unknown sysex is
    // ignored), and each family's two halves cover all four D-4T rows.
    static const uint8_t MCU_HDR[6] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12};
    static const uint8_t EXT_HDR[6] = {0xF0, 0x00, 0x02, 0x4E, 0x15, 0x13};
    blankHalf(MCU_HDR, 0x00);
    blankHalf(MCU_HDR, 0x38);
    blankHalf(EXT_HDR, 0x00);
    blankHalf(EXT_HDR, 0x38);
    // RGB backdrops dark (Icon whole-strip color frame: 8 x R,G,B zeros).
    {
        uint8_t frame[6 + 24 + 1];
        static const uint8_t COL_HDR[6] = {0xF0, 0x00, 0x02, 0x4E, 0x16, 0x14};
        memcpy(frame, COL_HDR, 6);
        memset(frame + 6, 0, 24);
        frame[sizeof(frame) - 1] = 0xF7;
        usbSysEx(frame, sizeof(frame));
    }
    // Strip/transport LEDs dark (MCU note range 0x00..0x73 on ch 1 -- through
    // the Zoom/Scrub and SMPTE/BEATS/Rude Solo LEDs, which stopped at 0x5F
    // until 1.7.2 -- plus the ch-2 alias range some firmwares route to strip
    // LEDs), vpot rings off, meters cleared (level, then overload), segment
    // display cleared.
    for (uint8_t n = 0; n <= 0x73; n++) usbShort(0x9, 0x90, n, 0x00);
    for (uint8_t n = 0; n <= 0x0F; n++) usbShort(0x9, 0x91, n, 0x00);
    for (uint8_t cc = 0x30; cc <= 0x37; cc++) usbShort(0xB, 0xB0, cc, 0x00);
    for (uint8_t ch = 0; ch < 8; ch++) usbShort(0xD, 0xD0, (uint8_t)(ch << 4), 0);
    for (uint8_t ch = 0; ch < 8; ch++) usbShort(0xD, 0xD0, (uint8_t)((ch << 4) | 0x0F), 0);
    for (uint8_t cc = 0x40; cc <= 0x4B; cc++) usbShort(0xB, 0xB0, cc, 0x00);
}
