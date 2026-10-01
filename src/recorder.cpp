#include "recorder.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "rtp_midi.h"
#include "usb_midi_host.h"
#include "wifi_net.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

namespace {
using namespace Recorder;

// 4 MB of the 8 MB PSRAM: 262 144 records. Without PSRAM (a board built
// without it, or the allocation failing) a 32 kB internal ring: 2048 records.
constexpr size_t PSRAM_BYTES = 4u << 20;
constexpr size_t INTERNAL_BYTES = 32u << 10;
constexpr uint64_t FREEZE_DELAY_US = 10ull * 1000 * 1000;
constexpr uint32_t TIME_EVERY_MS = 60000;
// The clock's high half, and the first record that carries it, for every
// 71.6-minute epoch the ring may still hold: fullTime() without a scan. 256
// epochs are 12.7 days; past that the oldest records' times are a guess.
constexpr int EPOCHS = 256;

Rec* s_ring = nullptr;
uint64_t s_mask = 0;  // capacity - 1, a power of two
bool s_psram = false;
uint8_t s_triggerMask = TRIGGERS_ALL;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Under s_mux from here on.
uint64_t s_seq = 0;  // newest record; 64-bit so it never wraps
bool s_frozen = false;
uint8_t s_trigger = TRIG_NONE;
uint64_t s_triggerUs = 0;
uint64_t s_freezeUs = 0;
uint32_t s_refused = 0;
struct Epoch {
    uint64_t firstSeq;
    uint32_t high;
};
Epoch s_epochs[EPOCHS];
int s_epochCount = 0;  // total pushed; the table keeps the newest EPOCHS

// Loop task only.
uint32_t s_lastSecMs = 0;
uint64_t s_lastSecSeq = 0;
volatile uint32_t s_perSec = 0;
uint32_t s_lastTimeMs = 0;
uint8_t s_loggedTrigger = TRIG_NONE;
bool s_loggedFrozen = false;

// Stores r (its seq and time filled in here, so both follow the order records
// are stored in). False when frozen. Caller holds s_mux.
bool storeLocked(Rec& r) {
    const uint64_t now = esp_timer_get_time();
    if (!s_frozen && s_trigger != TRIG_NONE && now >= s_freezeUs) s_frozen = true;
    if (s_frozen) {
        s_refused++;
        return false;
    }
    r.seq = (uint32_t)++s_seq;
    r.tUs = (uint32_t)now;
    const uint32_t high = (uint32_t)(now >> 32);
    // The decoder's anchors carry the high half of this very timestamp.
    if (r.kind == TIME || r.kind == ARM || r.kind == FREEZE) memcpy(r.data, &high, 4);
    if (!s_epochCount || s_epochs[(s_epochCount - 1) % EPOCHS].high != high) {
        s_epochs[s_epochCount % EPOCHS] = {s_seq, high};
        s_epochCount++;
    }
    s_ring[s_seq & s_mask] = r;
    return true;
}

// High half of the clock for record `seq`. Caller holds s_mux.
uint32_t highForLocked(uint64_t seq) {
    const int kept = s_epochCount < EPOCHS ? s_epochCount : EPOCHS;
    for (int i = 0; i < kept; i++) {
        const Epoch& e = s_epochs[(s_epochCount - 1 - i) % EPOCHS];
        if (seq >= e.firstSeq) return e.high;
    }
    return kept ? s_epochs[(s_epochCount - kept) % EPOCHS].high : 0;
}

uint64_t oldestLocked() {
    const uint64_t cap = s_mask + 1;
    return s_seq == 0 ? 0 : s_seq >= cap ? s_seq - cap + 1 : 1;
}

const char* const RESET_NAMES[] = {"UNKNOWN",  "POWERON",  "EXT",      "SW_RESTART",
                                   "PANIC",    "INT_WDT",  "TASK_WDT", "WDT",
                                   "DEEPSLEEP", "BROWNOUT", "SDIO"};
const char* const WEB_NAMES[] = {
    "other", "/", "/api/status", "/api/config", "/api/log", "/api/scan", "/config",
    "/config/export", "/config/import", "/update", "/reset", "/reboot", "/diag",
    "/diagreset", "/api/recorder", "/recorder.txt", "/api/recorder/arm|freeze"};
const char* const TRIGGER_NAMES[] = {"none", "session drop", "heartbeat stop",
                                     "USB port reset", "WiFi drop", "freeze now"};

// The packet as the status page's event logs print it, real-time and
// reserved packets included.
int formatPacket(const uint8_t* p, char* buf, size_t len) {
    const uint8_t cable = p[0] >> 4, cin = p[0] & 0x0F, ch = (p[1] & 0x0F) + 1;
    switch (cin) {
        case 0x8: return snprintf(buf, len, "c%u ch%u note off %u vel %u", cable, ch, p[2], p[3]);
        case 0x9: return snprintf(buf, len, "c%u ch%u note on %u vel %u", cable, ch, p[2], p[3]);
        case 0xA: return snprintf(buf, len, "c%u ch%u poly AT %u = %u", cable, ch, p[2], p[3]);
        case 0xB: return snprintf(buf, len, "c%u ch%u cc %u = %u", cable, ch, p[2], p[3]);
        case 0xC: return snprintf(buf, len, "c%u ch%u program %u", cable, ch, p[2]);
        case 0xD: return snprintf(buf, len, "c%u ch%u pressure %u", cable, ch, p[2]);
        case 0xE:
            return snprintf(buf, len, "c%u ch%u pitch %+d", cable, ch,
                            (p[2] | (p[3] << 7)) - 8192);
        case 0xF: return snprintf(buf, len, "c%u realtime %02x", cable, p[1]);
        case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
            return snprintf(buf, len, "c%u sys/sysex %02x %02x %02x", cable, p[1], p[2], p[3]);
        default:
            return snprintf(buf, len, "c%u reserved %02x %02x %02x %02x", cable, p[0], p[1],
                            p[2], p[3]);
    }
}

uint32_t le32(const uint8_t* d) {
    return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}
}  // namespace

void Recorder::begin(uint8_t triggerMask) {
    s_triggerMask = triggerMask & TRIGGERS_ALL;
    void* mem = heap_caps_malloc(PSRAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t bytes = PSRAM_BYTES;
    s_psram = mem != nullptr;
    if (!mem) {
        bytes = INTERNAL_BYTES;
        mem = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!mem) {
        Serial.println("[rec] no memory for the flight recorder -- it is off");
        return;
    }
    memset(mem, 0, bytes);
    s_mask = bytes / sizeof(Rec) - 1;
    s_ring = static_cast<Rec*>(mem);
    unsigned a = 0, b = 0, c = 0;
    sscanf(FW_VERSION, "%u.%u.%u", &a, &b, &c);
    const uint8_t ver[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
    put(BOOT, (uint8_t)esp_reset_reason(), ver, 3);
    put(TIME, 0);
    s_lastTimeMs = millis();
    Serial.printf("[rec] flight recorder: %lu records in %s\n", (unsigned long)(s_mask + 1),
                  s_psram ? "PSRAM" : "internal RAM");
}

void Recorder::put(Kind kind, uint8_t arg, const void* data, size_t len) {
    if (!s_ring) return;
    Rec r;
    r.kind = kind;
    r.arg = arg;
    memset(r.data, 0, sizeof(r.data));
    if (len) memcpy(r.data, data, len < sizeof(r.data) ? len : sizeof(r.data));
    portENTER_CRITICAL(&s_mux);
    storeLocked(r);
    portEXIT_CRITICAL(&s_mux);
}

void Recorder::trigger(Trigger t) {
    if (!s_ring || t == TRIG_NONE || t > TRIG_MANUAL) return;
    if (t != TRIG_MANUAL && !(s_triggerMask & (1u << (t - 1)))) return;
    Rec r = {};
    r.kind = FREEZE;
    r.arg = t;
    portENTER_CRITICAL(&s_mux);
    // "Freeze now" also cuts short another trigger's 10 s.
    const bool pending = s_trigger != TRIG_NONE;
    if (!s_frozen && (!pending || t == TRIG_MANUAL) && storeLocked(r)) {
        const uint64_t now = esp_timer_get_time();
        s_trigger = t;
        s_triggerUs = now;
        s_freezeUs = t == TRIG_MANUAL ? now : now + FREEZE_DELAY_US;
        if (t == TRIG_MANUAL) s_frozen = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

void Recorder::arm() {
    if (!s_ring) return;
    Rec r = {};
    r.kind = ARM;
    portENTER_CRITICAL(&s_mux);
    s_frozen = false;
    s_trigger = TRIG_NONE;
    s_triggerUs = s_freezeUs = 0;
    s_refused = 0;
    // Stored before the lock is let go, so it is the first record after the
    // freeze: with the clock's high half (storeLocked) it is the decoder's
    // anchor for everything that follows, however long the freeze lasted.
    storeLocked(r);
    portEXIT_CRITICAL(&s_mux);
}

void Recorder::tick() {
    if (!s_ring) return;
    const uint32_t ms = millis();
    if (ms - s_lastTimeMs >= TIME_EVERY_MS) {
        s_lastTimeMs = ms;
        put(TIME, 0);
    }
    if (ms - s_lastSecMs < 1000) return;
    s_lastSecMs = ms;
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    // A freeze whose 10 s ran out while nothing was being stored.
    if (!s_frozen && s_trigger != TRIG_NONE && now >= s_freezeUs) s_frozen = true;
    const uint64_t seq = s_seq;
    const uint8_t trig = s_trigger;
    const bool frozen = s_frozen;
    portEXIT_CRITICAL(&s_mux);
    s_perSec = (uint32_t)(seq - s_lastSecSeq);
    s_lastSecSeq = seq;
    // The loop task may print; the tasks that fire triggers may not.
    if (trig != s_loggedTrigger) {
        s_loggedTrigger = trig;
        if (trig != TRIG_NONE && !frozen) {
            Serial.printf("[rec] %s: freezing the flight recorder in 10 s\n", triggerName(trig));
        }
    }
    if (frozen != s_loggedFrozen) {
        s_loggedFrozen = frozen;
        Serial.printf(frozen ? "[rec] frozen (%s)\n" : "[rec] recording again\n",
                      triggerName(trig));
    }
}

void Recorder::status(Status& out) {
    out = {};
    if (!s_ring) return;
    out.enabled = true;
    out.psram = s_psram;
    out.capacity = (uint32_t)(s_mask + 1);
    out.perSec = s_perSec;
    portENTER_CRITICAL(&s_mux);
    out.seq = s_seq;
    out.oldest = oldestLocked();
    if (out.oldest) {
        const Rec& r = s_ring[out.oldest & s_mask];
        out.oldestUs = (uint64_t)highForLocked(out.oldest) << 32 | r.tUs;
    }
    out.trigger = s_trigger;
    out.frozen = s_frozen;
    out.triggerUs = s_triggerUs;
    out.freezeUs = s_freezeUs;
    out.refused = s_refused;
    portEXIT_CRITICAL(&s_mux);
    if (out.frozen) out.perSec = 0;
}

size_t Recorder::read(uint64_t from, Rec* out, size_t max, uint64_t& next, uint64_t& lost) {
    lost = 0;
    size_t n = 0;
    if (!s_ring) {
        next = from;
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    const uint64_t oldest = oldestLocked();
    if (oldest && from < oldest) {
        lost = from ? oldest - from : 0;
        from = oldest;
    }
    if (!from) from = 1;
    while (n < max && from <= s_seq) out[n++] = s_ring[from++ & s_mask];
    portEXIT_CRITICAL(&s_mux);
    next = from;
    return n;
}

uint64_t Recorder::fullTime(const Rec& r, uint64_t seq) {
    portENTER_CRITICAL(&s_mux);
    const uint32_t high = highForLocked(seq);
    portEXIT_CRITICAL(&s_mux);
    return (uint64_t)high << 32 | r.tUs;
}

uint64_t Recorder::seqAt(uint64_t us) {
    if (!s_ring) return 0;
    // Times rise with seq, so a binary search over what the ring holds.
    portENTER_CRITICAL(&s_mux);
    uint64_t lo = oldestLocked(), hi = s_seq;
    portEXIT_CRITICAL(&s_mux);
    if (!lo) return 0;
    uint64_t found = lo - 1;
    while (lo <= hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        Rec r;
        uint64_t next, lost;
        if (read(mid, &r, 1, next, lost) != 1 || lost) {
            lo = next;  // overwritten meanwhile: the ring moved on
            continue;
        }
        if (fullTime(r, mid) <= us) {
            found = mid;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }
    return found;
}

const char* Recorder::triggerName(uint8_t t) {
    return t <= TRIG_MANUAL ? TRIGGER_NAMES[t] : "?";
}

size_t Recorder::format(const Rec& r, uint64_t tFullUs, char* buf, size_t len) {
    int n = snprintf(buf, len, "%lu.%06lu ", (unsigned long)(tFullUs / 1000000),
                     (unsigned long)(tFullUs % 1000000));
    if (n < 0 || (size_t)n >= len) return len ? len - 1 : 0;
    char* p = buf + n;
    size_t room = len - n;
    const uint8_t* d = r.data;
    char pkt[48];
    switch (r.kind) {
        case BOOT:
            n = snprintf(p, room, "boot reset=%s fw %u.%u.%u",
                         r.arg < sizeof(RESET_NAMES) / sizeof(RESET_NAMES[0])
                             ? RESET_NAMES[r.arg] : "?",
                         d[0], d[1], d[2]);
            break;
        case TIME: n = snprintf(p, room, "time"); break;
        case USB_IN:
            formatPacket(d, pkt, sizeof(pkt));
            n = snprintf(p, room, "usb_in %s%s%s%s%s", pkt,
                         r.arg & IN_FORWARDED ? " fwd" : "",
                         r.arg & IN_OTHER_CABLE ? " other-cable" : "",
                         r.arg & IN_NO_PEER ? " no-peer" : "", r.arg ? "" : " held");
            break;
        case USB_OUT:
            formatPacket(d, pkt, sizeof(pkt));
            n = snprintf(p, room, "usb_out %s %s", pkt, r.arg & OUT_QUEUED ? "queued" : "DROPPED");
            break;
        case UPLINK:
            n = snprintf(p, room, "uplink %u%s packets, %u tokens left", r.arg,
                         r.arg == 255 ? "+" : "", d[0]);
            break;
        case HEARTBEAT:
            n = r.arg ? snprintf(p, room, "heartbeat held: %s", UsbMidi::healthReasonName(r.arg))
                      : snprintf(p, room, "heartbeat sent");
            break;
        case SESSION:
            switch (r.arg) {
                case SESSION_CONNECTED:
                case SESSION_DISCONNECTED:
                case SESSION_RECONNECTED:
                    n = snprintf(p, room, "session %s peers=%u ssrc=%08lx",
                                 r.arg == SESSION_CONNECTED      ? "connected"
                                 : r.arg == SESSION_DISCONNECTED ? "disconnected"
                                                                 : "reconnected",
                                 d[0], (unsigned long)le32(d + 2));
                    break;
                case SESSION_EXCEPTION:
                    n = snprintf(p, room, "session exception %s value=%ld peers=%u",
                                 RtpMidi::exceptionName(d[1]), (long)(int32_t)le32(d + 2), d[0]);
                    break;
                case SESSION_INVITE: n = snprintf(p, room, "session invite sent"); break;
                default: n = snprintf(p, room, "session ?%u", r.arg); break;
            }
            break;
        case MARKER:
            n = snprintf(p, room, "marker %s", r.arg ? "attached" : "detached");
            break;
        case USB_STATE:
            switch (r.arg) {
                case USB_ATTACH:
                    n = snprintf(p, room, "usb attach %04x:%04x if %u", d[0] | d[1] << 8,
                                 d[2] | d[3] << 8, d[4]);
                    break;
                case USB_DETACH: n = snprintf(p, room, "usb detach"); break;
                case USB_EP_CLEAR: n = snprintf(p, room, "usb ep_clear 0x%02x", d[0]); break;
                case USB_PORT_RESET:
                    n = snprintf(p, room, "usb port_reset (%s)",
                                 d[0] == 1 ? "errors persist" : "device stopped accepting");
                    break;
                case USB_ENUM_RETRY: n = snprintf(p, room, "usb enum_retry"); break;
                case USB_RX_ERROR: n = snprintf(p, room, "usb rx_error status %u", d[0]); break;
                case USB_TX_ERROR:
                    n = snprintf(p, room, "usb tx_error status %u lost %u", d[0], d[1]);
                    break;
                default: n = snprintf(p, room, "usb ?%u", r.arg); break;
            }
            break;
        case WIFI:
            switch (r.arg) {
                case WIFI_GOT_IP: n = snprintf(p, room, "wifi got_ip rssi %d", (int8_t)d[1]); break;
                case WIFI_DISCONNECTED:
                    n = snprintf(p, room, "wifi disconnected reason %s rssi %d",
                                 WifiNet::reasonText(d[0]).c_str(), (int8_t)d[1]);
                    break;
                case WIFI_LOST_IP: n = snprintf(p, room, "wifi lost_ip"); break;
                case WIFI_RETRY: n = snprintf(p, room, "wifi retry"); break;
                case WIFI_KICK: n = snprintf(p, room, "wifi kick"); break;
                case WIFI_RSSI: n = snprintf(p, room, "wifi rssi %d", (int8_t)d[1]); break;
                default: n = snprintf(p, room, "wifi ?%u", r.arg); break;
            }
            break;
        case TASK:
            n = snprintf(p, room, "task pass %lu us, %u messages", (unsigned long)le32(d), d[4]);
            break;
        case WEB:
            n = snprintf(p, room, "web %s %lu us",
                         r.arg < sizeof(WEB_NAMES) / sizeof(WEB_NAMES[0]) ? WEB_NAMES[r.arg] : "?",
                         (unsigned long)le32(d));
            break;
        case FREEZE: n = snprintf(p, room, "freeze trigger=%s", triggerName(r.arg)); break;
        case ARM: n = snprintf(p, room, "arm"); break;
        default: n = snprintf(p, room, "kind %u arg %u", r.kind, r.arg); break;
    }
    if (n < 0) n = 0;
    const size_t used = (size_t)(p - buf) + ((size_t)n < room ? (size_t)n : room - 1);
    return used;
}
