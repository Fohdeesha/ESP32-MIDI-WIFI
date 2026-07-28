#include "midi_bridge.h"

#include <cstring>

#include "rtp_midi.h"
#include "usb_midi_host.h"

// USB -> network direction only for now. The reverse direction (RTP in ->
// USB out) is 0.7.0.
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
