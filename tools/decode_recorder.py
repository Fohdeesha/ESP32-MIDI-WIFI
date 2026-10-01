"""Decode a flight recorder download from ESP32-MIDI-WIFI.

usage: python decode_recorder.py FILE

FILE is what GET /api/recorder returns ("Download all" on the status page).
Prints one line per record, the same lines /recorder.txt gives:
"<seconds since boot> <what happened>". Records overwritten while the download
ran show up as a gap: "... N records lost".

The format is documented in src/recorder.h: a 40-byte header, then 16-byte
records, little-endian.
"""

import struct
import sys

# magic, version, record size, first seq, capacity, the clock at download,
# the first record's full time
HEADER = struct.Struct("<8sIIIIQQ")
REC = struct.Struct("<IIBB6s")      # seq, low 32 bits of the clock, kind, arg, data

KINDS = {1: "boot", 2: "time", 3: "usb_in", 4: "usb_out", 5: "uplink", 6: "heartbeat",
         7: "session", 8: "marker", 9: "usb", 10: "wifi", 11: "task", 12: "web",
         13: "freeze", 14: "arm"}
RESET_NAMES = ["UNKNOWN", "POWERON", "EXT", "SW_RESTART", "PANIC", "INT_WDT", "TASK_WDT",
               "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO"]
WEB_NAMES = ["other", "/", "/api/status", "/api/config", "/api/log", "/api/scan", "/config",
             "/config/export", "/config/import", "/update", "/reset", "/reboot", "/diag",
             "/diagreset", "/api/recorder", "/recorder.txt", "/api/recorder/arm|freeze"]
TRIGGER_NAMES = ["none", "session drop", "heartbeat stop", "USB port reset", "WiFi drop",
                 "freeze now"]
HEALTH_NAMES = {1: "no device", 2: "port reset pending", 3: "IN pipeline dead",
                4: "OUT pipe halted", 5: "OUT transfer unacknowledged over 2 s"}
EXCEPTION_NAMES = ["BufferFull", "Parse", "UnexpectedParse", "TooManyParticipants",
                   "ComputerNotInDirectory", "NotAcceptingAnyone", "UnexpectedInvite",
                   "ParticipantNotFound", "ListenerTimeOut", "MaxAttempts",
                   "NoResponseFromConnectionRequest", "SendPacketsDropped",
                   "ReceivedPacketsDropped", "UdpBeginPacketFailed"]
WIFI_REASONS = {2: "AUTH_EXPIRE", 3: "AUTH_LEAVE", 4: "ASSOC_EXPIRE", 5: "ASSOC_TOOMANY",
                8: "STA_LEAVING", 15: "4WAY_HANDSHAKE_TIMEOUT", 200: "BEACON_TIMEOUT",
                201: "NO_AP_FOUND", 202: "AUTH_FAIL", 203: "ASSOC_FAIL",
                204: "HANDSHAKE_TIMEOUT", 205: "CONNECTION_FAIL"}


def le32(b):
    return struct.unpack_from("<I", b)[0]


def s8(v):
    return v - 256 if v > 127 else v


def packet(p):
    cable, cin, ch = p[0] >> 4, p[0] & 0x0F, (p[1] & 0x0F) + 1
    if cin == 0x8:
        return "c%u ch%u note off %u vel %u" % (cable, ch, p[2], p[3])
    if cin == 0x9:
        return "c%u ch%u note on %u vel %u" % (cable, ch, p[2], p[3])
    if cin == 0xA:
        return "c%u ch%u poly AT %u = %u" % (cable, ch, p[2], p[3])
    if cin == 0xB:
        return "c%u ch%u cc %u = %u" % (cable, ch, p[2], p[3])
    if cin == 0xC:
        return "c%u ch%u program %u" % (cable, ch, p[2])
    if cin == 0xD:
        return "c%u ch%u pressure %u" % (cable, ch, p[2])
    if cin == 0xE:
        return "c%u ch%u pitch %+d" % (cable, ch, (p[2] | p[3] << 7) - 8192)
    if cin == 0xF:
        return "c%u realtime %02x" % (cable, p[1])
    if 0x2 <= cin <= 0x7:
        return "c%u sys/sysex %02x %02x %02x" % (cable, p[1], p[2], p[3])
    return "c%u reserved %02x %02x %02x %02x" % (cable, p[0], p[1], p[2], p[3])


def describe(kind, arg, d):
    if kind == 1:
        return "boot reset=%s fw %u.%u.%u" % (
            RESET_NAMES[arg] if arg < len(RESET_NAMES) else "?", d[0], d[1], d[2])
    if kind == 2:
        return "time"
    if kind == 3:
        flags = ((" fwd" if arg & 1 else "") + (" other-cable" if arg & 2 else "")
                 + (" no-peer" if arg & 4 else "") + ("" if arg else " held"))
        return "usb_in " + packet(d) + flags
    if kind == 4:
        return "usb_out %s %s" % (packet(d), "queued" if arg & 1 else "DROPPED")
    if kind == 5:
        return "uplink %u%s packets, %u tokens left" % (arg, "+" if arg == 255 else "", d[0])
    if kind == 6:
        return "heartbeat held: %s" % HEALTH_NAMES.get(arg, "?") if arg else "heartbeat sent"
    if kind == 7:
        if arg in (1, 2, 3):
            what = {1: "connected", 2: "disconnected", 3: "reconnected"}[arg]
            return "session %s peers=%u ssrc=%08x" % (what, d[0], le32(d[2:6]))
        if arg == 4:
            name = EXCEPTION_NAMES[d[1]] if d[1] < len(EXCEPTION_NAMES) else "?"
            value = struct.unpack_from("<i", d, 2)[0]
            return "session exception %s value=%d peers=%u" % (name, value, d[0])
        if arg == 5:
            return "session invite sent"
        return "session ?%u" % arg
    if kind == 8:
        return "marker %s" % ("attached" if arg else "detached")
    if kind == 9:
        if arg == 1:
            return "usb attach %04x:%04x if %u" % (d[0] | d[1] << 8, d[2] | d[3] << 8, d[4])
        if arg == 2:
            return "usb detach"
        if arg == 3:
            return "usb ep_clear 0x%02x" % d[0]
        if arg == 4:
            return "usb port_reset (%s)" % ("errors persist" if d[0] == 1
                                            else "device stopped accepting")
        if arg == 5:
            return "usb enum_retry"
        if arg == 6:
            return "usb rx_error status %u" % d[0]
        if arg == 7:
            return "usb tx_error status %u lost %u" % (d[0], d[1])
        return "usb ?%u" % arg
    if kind == 10:
        if arg == 1:
            return "wifi got_ip rssi %d" % s8(d[1])
        if arg == 2:
            name = WIFI_REASONS.get(d[0])
            reason = "%s (%u)" % (name, d[0]) if name else "%u" % d[0]
            return "wifi disconnected reason %s rssi %d" % (reason, s8(d[1]))
        if arg in (3, 4, 5):
            return "wifi " + {3: "lost_ip", 4: "retry", 5: "kick"}[arg]
        if arg == 6:
            return "wifi rssi %d" % s8(d[1])
        return "wifi ?%u" % arg
    if kind == 11:
        return "task pass %u us, %u messages" % (le32(d), d[4])
    if kind == 12:
        return "web %s %u us" % (WEB_NAMES[arg] if arg < len(WEB_NAMES) else "?", le32(d))
    if kind == 13:
        return "freeze trigger=%s" % (TRIGGER_NAMES[arg] if arg < len(TRIGGER_NAMES) else "?")
    if kind == 14:
        return "arm"
    return "kind %u arg %u" % (kind, arg)


def full_times(recs, first_us):
    """Each record's full microsecond time. Times rise with the record order.
    The header gives the first record's; BOOT (high half 0), TIME, FREEZE and
    ARM records carry the clock's high half; between them a fall in the low
    half is a wrap (every 71.6 min)."""
    out = []
    high = first_us >> 32
    prev = None
    for _, t, kind, _, d in recs:
        if kind == 1:
            high = 0
        elif kind in (2, 13, 14):
            high = le32(d)
        elif prev is not None and t < prev:
            high += 1
        out.append(high << 32 | t)
        prev = t
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    if len(raw) < HEADER.size:
        sys.exit("too short for a recorder download")
    magic, version, size, _, _, _, first_us = HEADER.unpack_from(raw)
    if magic != b"ESPMREC1" or version != 1 or size != REC.size:
        sys.exit("not a version 1 recorder download")
    body = raw[HEADER.size:]
    # A download cut short ends mid-record; that part is dropped.
    recs = [REC.unpack_from(body, o) for o in range(0, len(body) - len(body) % REC.size, REC.size)]
    times = full_times(recs, first_us)
    out = sys.stdout
    prev_seq = None
    for (seq, _, kind, arg, d), t in zip(recs, times):
        if prev_seq is not None and (seq - prev_seq) & 0xFFFFFFFF != 1:
            out.write("... %u records lost\n" % (((seq - prev_seq) & 0xFFFFFFFF) - 1))
        prev_seq = seq
        out.write("%u.%06u %s\n" % (t // 1000000, t % 1000000, describe(kind, arg, d)))


if __name__ == "__main__":
    main()
