#pragma once

#include <WString.h>

#include <cstddef>
#include <cstdint>

// Flight recorder (1.11.0): every USB-MIDI packet in both directions and every
// link event, timestamped to the microsecond, in a 4 MB ring in PSRAM -- about
// 262 000 records. That is minutes of a busy fader stream and hours of an idle
// one. When something goes wrong (a session drop, the heartbeat stopping, a USB
// port reset, a WiFi drop) the ring keeps recording 10 s more and then freezes,
// so the moments around the glitch are not overwritten before anyone looks.
// POST /api/recorder/arm resumes it.
//
// Any task may write: each record is claimed and copied under one spinlock,
// about a microsecond. The timestamp is taken inside that lock, so times rise
// with seq across all tasks -- seqAt()'s binary search and the decoder's wrap
// handling depend on it, so keep the clock read there.
//
// Download format (GET /api/recorder, read by tools/decode_recorder.py): a
// 40-byte header, then records, all little-endian.
//   header: char magic[8] = "ESPMREC1"; uint32 version = 1; uint32 record size
//           = 16; uint32 first seq sent (low 32 bits); uint32 capacity in
//           records; uint64 the microsecond clock when the download started;
//           uint64 the full time of the first record sent.
//   record: Rec below. Records carry their own seq, so a gap in it marks
//           records overwritten while the download ran. BOOT (high half 0),
//           TIME, FREEZE and ARM records carry the clock's high half, the
//           first record's time is in the header, and between those a fall in
//           tUs is a wrap.
namespace Recorder {
struct Rec {
    uint32_t seq;   // 1, 2, 3 ... since boot
    uint32_t tUs;   // esp_timer_get_time(), low 32 bits (wraps every 71.6 min;
                    // TIME records carry the high half)
    uint8_t kind;   // Kind
    uint8_t arg;    // per kind, below
    uint8_t data[6];
};
static_assert(sizeof(Rec) == 16, "the download format depends on it");

enum Kind : uint8_t {
    BOOT = 1,       // arg = esp_reset_reason(); data[0..2] = firmware version
    TIME = 2,       // data[0..3] = the clock's high 32 bits (every 60 s)
    USB_IN = 3,     // arg = IN_* flags; data[0..3] = the USB-MIDI event packet
    USB_OUT = 4,    // arg = OUT_* flags; data[0..3] = the packet
    UPLINK = 5,     // one datagram to the network: arg = USB packets in it
                    // (255 = 255 or more), data[0] = pacing tokens left
    HEARTBEAT = 6,  // arg = 0 sent, else the UsbMidi::healthReason() that held it
    SESSION = 7,    // arg = SESSION_*; data[0] = peers after; data[1] = exception
                    // number; data[2..5] = the peer's SSRC, or the exception value
    MARKER = 8,     // attach/detach marker sent: arg = 1 attached, 0 detached
    USB_STATE = 9,  // arg = USB_*; data per event (see recorder.cpp's decoder)
    WIFI = 10,      // arg = WIFI_*; data[0] = reason code; data[1] = RSSI (int8)
    TASK = 11,      // a MIDI task pass over 5 ms: data[0..3] = its length in us,
                    // data[4] = messages the RTP drain handled in it
    WEB = 12,       // a web request: arg = WEB_* path; data[0..3] = time in us
    FREEZE = 13,    // a trigger fired: arg = Trigger; recording stops 10 s later
                    // ("freeze now": at once); data[0..3] = the clock's high 32 bits
    ARM = 14,       // recording resumed; data[0..3] = the clock's high 32 bits
};

// USB_IN flags: what the bridge did with the packet.
constexpr uint8_t IN_FORWARDED = 1;   // went to the network
constexpr uint8_t IN_OTHER_CABLE = 2; // not the bridged cable
constexpr uint8_t IN_NO_PEER = 4;     // nobody connected to send it to
// USB_OUT flags.
constexpr uint8_t OUT_QUEUED = 1;     // else dropped (queue full or device stuck)

enum SessionEvent : uint8_t {
    SESSION_CONNECTED = 1,
    SESSION_DISCONNECTED = 2,
    SESSION_RECONNECTED = 3,  // a repeated IN/OK from a peer already connected
    SESSION_EXCEPTION = 4,
    SESSION_INVITE = 5,       // invitation sent to the configured peer
};

enum UsbEvent : uint8_t {
    USB_ATTACH = 1,      // claimed: data[0..1] = VID, data[2..3] = PID, data[4] = iface
    USB_DETACH = 2,
    USB_EP_CLEAR = 3,    // data[0] = endpoint address
    USB_PORT_RESET = 4,  // data[0] = 1 errors persist, 2 device stopped accepting
    USB_ENUM_RETRY = 5,
    USB_RX_ERROR = 6,    // data[0] = usb_transfer_status_t
    USB_TX_ERROR = 7,    // data[0] = status; data[1] = packets lost
};

enum WifiEvent : uint8_t {
    WIFI_GOT_IP = 1,
    WIFI_DISCONNECTED = 2,  // data[0] = reason
    WIFI_LOST_IP = 3,
    WIFI_RETRY = 4,
    WIFI_KICK = 5,
    WIFI_RSSI = 6,          // a sample, every 2 s while connected
};

enum WebPath : uint8_t {
    WEB_OTHER = 0, WEB_ROOT, WEB_API_STATUS, WEB_API_CONFIG, WEB_API_LOG, WEB_API_SCAN,
    WEB_CONFIG, WEB_EXPORT, WEB_IMPORT, WEB_UPDATE, WEB_RESET, WEB_REBOOT, WEB_DIAG,
    WEB_DIAGRESET, WEB_RECORDER, WEB_RECORDER_TXT, WEB_RECORDER_CTL,
};

// What can freeze the ring. The first four are switched by the rtrig setting,
// bit (1 << (trigger - 1)); MANUAL ("Freeze now") always freezes at once.
enum Trigger : uint8_t {
    TRIG_NONE = 0,
    TRIG_SESSION = 1,    // a connected peer's session ended
    TRIG_HEARTBEAT = 2,  // the heartbeat stopped for over 1 s with a peer connected
    TRIG_USB_RESET = 3,  // the device's port was reset to recover it
    TRIG_WIFI = 4,       // the station lost its AP
    TRIG_MANUAL = 5,
};
constexpr uint8_t TRIGGERS_ALL = 0x0F;

// Allocates the ring (PSRAM, else a small internal one) and writes BOOT.
// Call first thing in setup(); every put() before it is ignored.
void begin(uint8_t triggerMask);
// Stores one record. Any task, never blocks; ignored while frozen.
void put(Kind kind, uint8_t arg, const void* data = nullptr, size_t len = 0);
// The same with the 4 bytes of a USB-MIDI packet.
inline void putPacket(Kind kind, uint8_t arg, const uint8_t pkt[4]) { put(kind, arg, pkt, 4); }
// A trigger fired: unless it is switched off or the ring is already frozen or
// freezing, recording stops 10 s from now. MANUAL stops it at once, also while
// another trigger's 10 s are running. Any task.
void trigger(Trigger t);
void arm();  // resume recording
// Loop task, once per pass: the TIME record, the records-per-second figure,
// and a freeze whose 10 s ran out while nothing was being recorded.
void tick();

// Sequence numbers in this API are 64-bit and never wrap; a Rec carries the
// low 32 bits of its own.
struct Status {
    bool enabled;         // a ring was allocated
    bool psram;
    uint32_t capacity;    // records
    uint64_t seq;         // newest record
    uint64_t oldest;      // oldest record still held (0 = none)
    uint64_t oldestUs;    // its time
    uint32_t perSec;      // records stored in the last second
    uint8_t trigger;      // what froze it, or is freezing it (TRIG_NONE = recording)
    bool frozen;          // stopped; else pending if trigger != TRIG_NONE
    uint64_t triggerUs;   // when the trigger fired
    uint64_t freezeUs;    // when recording stopped, or will
    uint32_t refused;     // records refused while frozen, since the last arm()
};
void status(Status& out);

// Copies up to max records from seq `from` on into out (keep max small: the
// copy holds the lock). Records already overwritten are skipped: `lost` says
// how many, and the copy starts at the oldest one held (from = 0: the oldest,
// not counted as lost). Returns the count; `next` is the seq to ask for next.
size_t read(uint64_t from, Rec* out, size_t max, uint64_t& next, uint64_t& lost);
// The newest record at or before clock time `us`, 0 if none -- where a
// "last N seconds" view starts (it starts after this one).
uint64_t seqAt(uint64_t us);
// A record's full 64-bit time, from its seq.
uint64_t fullTime(const Rec& r, uint64_t seq);
// One record as a line of text (no newline), as tools/decode_recorder.py
// prints it. tFullUs is the record's full time.
size_t format(const Rec& r, uint64_t tFullUs, char* buf, size_t len);
const char* triggerName(uint8_t t);
}  // namespace Recorder
