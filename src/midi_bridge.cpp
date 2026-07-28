#include "midi_bridge.h"

#include <Arduino.h>

#include <cstring>

#include "rtp_midi.h"
#include "usb_midi_host.h"

// USB -> network direction first, RTP -> USB below.
namespace {

// Only the P1-M's first virtual MIDI cable carries the control surface;
// the other three are unused ports we deliberately don't bridge.
constexpr uint8_t BRIDGED_CABLE = 0;

uint32_t s_forwarded = 0;

// SysEx arrives as 3-byte chunks (CIN 0x4) closed by a 1/2/3-byte end
// chunk (CIN 0x5-0x7); reassemble before handing it to the MIDI library.
// 256 bytes covers every MCU message a control surface sends host-ward.
uint8_t s_sysex[256];
uint16_t s_sysexLen = 0;
bool s_sysexOverflow = false;

void sysexAppend(const uint8_t* bytes, uint8_t n) {
    if (s_sysexLen + n > sizeof(s_sysex)) {
        s_sysexOverflow = true;
        return;
    }
    memcpy(s_sysex + s_sysexLen, bytes, n);
    s_sysexLen += n;
}

void sysexEnd(const uint8_t* bytes, uint8_t n) {
    sysexAppend(bytes, n);
    if (!s_sysexOverflow && s_sysexLen >= 2 && s_sysex[0] == 0xF0) {
        RtpMidi::sendSysEx(s_sysex, s_sysexLen);
        s_forwarded++;
    }
    s_sysexLen = 0;
    s_sysexOverflow = false;
}

}  // namespace

void MidiBridge::tick() {
    uint8_t p[4];
    while (UsbMidi::readPacket(p)) {
        if ((p[0] >> 4) != BRIDGED_CABLE) continue;
        if (!RtpMidi::hasPeer()) continue;
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
            case 0x4:  // sysex start/continue
                sysexAppend(&p[1], 3);
                break;
            case 0x5:  // sysex end, 1-3 bytes
                sysexEnd(&p[1], 1);
                break;
            case 0x6:
                sysexEnd(&p[1], 2);
                break;
            case 0x7:
                sysexEnd(&p[1], 3);
                break;
            default:  // system common/realtime: nothing the bridge needs yet
                break;
        }
    }
}

uint32_t MidiBridge::forwardedCount() {
    return s_forwarded;
}

// ---- RTP -> USB (0.7.0) ----------------------------------------------------
// Everything goes out on virtual cable 0, mirroring the inbound filter.

namespace {
uint32_t s_returned = 0;

void usbSend(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t pkt[4] = {cin, status, d1, d2};  // cable 0: high nibble stays 0
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
    // Chunk into 3-byte packets: CIN 0x4 continues, 0x5/0x6/0x7 end with
    // 1/2/3 bytes. Counted as one event regardless of packet count.
    if (length < 2) return;
    bool ok = true;
    uint16_t i = 0;
    while (length - i > 3) {
        uint8_t pkt[4] = {0x4, data[i], data[i + 1], data[i + 2]};
        ok &= UsbMidi::writePacket(pkt);
        i += 3;
    }
    uint8_t rem = length - i;
    uint8_t pkt[4] = {(uint8_t)(0x4 + rem), 0, 0, 0};
    memcpy(&pkt[1], &data[i], rem);
    ok &= UsbMidi::writePacket(pkt);
    if (ok) s_returned++;
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
// Chunk one complete sysex (F0..F7) into USB-MIDI event packets on cable 0.
void usbSysEx(const uint8_t* data, uint16_t length) {
    uint16_t i = 0;
    while (length - i > 3) {
        uint8_t pkt[4] = {0x4, data[i], data[i + 1], data[i + 2]};
        UsbMidi::writePacket(pkt);
        i += 3;
    }
    uint8_t rem = length - i;
    uint8_t pkt[4] = {(uint8_t)(0x4 + rem), 0, 0, 0};
    memcpy(&pkt[1], &data[i], rem);
    UsbMidi::writePacket(pkt);
}

void usbShort(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t pkt[4] = {cin, status, d1, d2};
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
    // Strip/transport LEDs dark (MCU note range 0x00..0x5F on ch 1, plus the
    // ch-2 alias range some firmwares route to strip LEDs), vpot rings off,
    // meters cleared, segment display cleared.
    for (uint8_t n = 0; n <= 0x5F; n++) usbShort(0x9, 0x90, n, 0x00);
    for (uint8_t n = 0; n <= 0x0F; n++) usbShort(0x9, 0x91, n, 0x00);
    for (uint8_t cc = 0x30; cc <= 0x37; cc++) usbShort(0xB, 0xB0, cc, 0x00);
    for (uint8_t ch = 0; ch < 8; ch++) usbShort(0xD, 0xD0, (uint8_t)(ch << 4), 0);
    for (uint8_t cc = 0x40; cc <= 0x4B; cc++) usbShort(0xB, 0xB0, cc, 0x00);
}
