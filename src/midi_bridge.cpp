#include "midi_bridge.h"

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
