"""Patch lathoub/AppleMIDI 3.3.0 at build time.

3.3.0 (Aug 2022) is the library's only published release, and every change
below fixes a bug in it that this bridge hit or can hit. Applied as a `pre:`
extra_script so a clean CI checkout is patched too -- the dependency stays
declared and updatable rather than being vendored.

Each patch replaces one exact block of library source. The script is
idempotent, and it FAILS THE BUILD if a block it expects is absent (the
library changed) or carries an older version of a patch (a stale patched copy:
delete .pio/libdeps and rebuild). When every patch is in place it stamps
AppleMIDI.h with ESP32_MIDI_WIFI_APPLEMIDI_PATCHSET, and src/rtp_midi.cpp
refuses to compile unless that stamp matches PATCHSET below -- so the firmware
can never be built against an unpatched library, whatever order PlatformIO
runs things in. Bump PATCHSET (and the check in rtp_midi.cpp) whenever a patch
is added or changed.

The patches:

sysex-trace -- rtpMIDI_Parser_CommandSection.hpp::decodeMidiSysEx() printed
  every SysEx byte to `Serial` with four unconditional `Serial.print` calls
  (left-over debug code, not behind any switch). Arduino-ESP32 leaves
  `HardwareSerial` with no TX ring buffer, so `uart_write_bytes` blocks at
  wire rate once the 128-byte FIFO is full: ~434 us of BLOCKING UART time per
  SysEx byte at 115200 baud. Measured 2026-07-28 on a control-surface rig:
  parseDataPackets() took up to 206 ms for ONE 945-byte datagram, loop() fell
  to ~4 Hz, the USB->RTP fader stream arrived in four clumps a second and 6%
  of outbound RTP packets were lost to the bursts.

listener-timeout -- with APPLEMIDI_INITIATOR defined (this build needs it for
  sendInvite), manageSynchronization() skipped every participant not in state
  Connected. Only sessions this device opens ever reach Connected, so a
  session the DAW opened was never checked against CK_MaxTimeOut: a peer that
  vanished without a BY (Mac asleep, crashed, out of range) stayed "connected"
  forever -- no surface blanking, a lit session LED, heartbeats sent to a dead
  address, and after two such losses both participant slots were full and
  every new invitation was refused until reboot.

reinvite, name-nul -- a repeated IN from a peer already in the list was
  silently ignored. The peer is listed BEFORE the OK goes out, so if that OK
  is lost on the air every retry from the peer is dropped and it can never
  finish the handshake (with the old timeout bug, never at all). A repeat is
  now answered again. Also: a 24-character session name was copied without
  its terminator.

journal -- the recovery-journal parser read the header of the FIRST channel
  journal only (a flag was never reset between channels), so channel journals
  2..N were left in the buffer and parsed as if they were a new packet; and
  the 10-bit channel-journal length was truncated to 8 bits.

datagram -- the parsers treated the socket as a byte stream: whatever a
  datagram left unparsed (a truncated or malformed packet, trailing bytes, a
  length field that overshoots) stayed in the buffer with the parser mid-
  packet, and the NEXT datagram was parsed as its continuation. One bad packet
  could so swallow the session's clock-sync traffic until the peer gave up.
  UDP carries exactly one RTP packet or AppleMIDI command per datagram, so
  each datagram is now parsed on its own and whatever it leaves is dropped
  with it.

seqwrap -- the sequence-number checks promoted uint16 to int, so every
  sequence wrap (each 65536 packets) logged a phantom ReceivedPacketsDropped
  (-65536) or SendPacketsDropped exception.
"""

import os
import re
import sys

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

PATCHSET = 2

LIB = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),  # noqa: F821
    env.subst("$PIOENV"),  # noqa: F821
    "AppleMIDI",
    "src",
)

STAMP_FILE = "AppleMIDI.h"
STAMP_RE = re.compile(r"#define ESP32_MIDI_WIFI_APPLEMIDI_PATCHSET [^\n]*\n")
STAMP = (
    "#define ESP32_MIDI_WIFI_APPLEMIDI_PATCHSET %d  "
    "// written by tools/patch_applemidi.py\n" % PATCHSET
)


class Patch(object):
    def __init__(self, name, filename, old, new, legacy=()):
        self.name = name
        self.filename = filename
        self.old = old
        self.new = new
        # Earlier forms of this patch (from older versions of this script)
        # that are upgraded in place rather than reported as stale.
        self.legacy = legacy
        tag = "[patched:%s v" % name
        if tag not in new:
            raise ValueError("patch %s: replacement lacks its %r tag" % (name, tag))
        self.tag_prefix = "[patched:%s " % name


PATCHES = [
    Patch(
        "sysex-trace",
        "rtpMIDI_Parser_CommandSection.hpp",
        """        Serial.print("0x");
        Serial.print(octet < 16 ? "0" : "");
        Serial.print(octet, HEX);
        Serial.print(" ");
""",
        """        // [patched:sysex-trace v1] per-byte SysEx Serial tracing removed
""",
        legacy=("""        // [patched] per-byte SysEx Serial tracing removed
""",),
    ),
    Patch(
        "listener-timeout",
        "AppleMIDI.hpp",
        """#ifdef APPLEMIDI_INITIATOR
        if (pParticipant->invitationStatus != Connected)
            continue;
""",
        """#ifdef APPLEMIDI_INITIATOR
        // [patched:listener-timeout v1] Only a session this device initiated
        // waits here for its handshake; a Listener never reaches Connected,
        // and skipping it switched its CK_MaxTimeOut check off for good.
        if (pParticipant->kind == Initiator
        &&  pParticipant->invitationStatus != Connected)
            continue;
""",
    ),
    Patch(
        "reinvite",
        "AppleMIDI.hpp",
        """    // ignore invitation of a participant already in the participant list
#ifndef ONE_PARTICIPANT
    if (nullptr != getParticipantBySSRC(invitation.ssrc))
#else
    if (participant.ssrc == invitation.ssrc)
#endif
        return;
""",
        """    // [patched:reinvite v1] A repeated IN from a participant already in
    // the list means our OK never arrived: answer it again (ignoring it left
    // the peer unable to finish the handshake).
#ifndef ONE_PARTICIPANT
    if (auto pKnown = getParticipantBySSRC(invitation.ssrc))
    {
        if (pKnown->kind == Listener)
        {
            pKnown->remoteIP   = controlPort.remoteIP();
            pKnown->remotePort = controlPort.remotePort();
            pKnown->lastSyncExchangeTime = now;
#ifdef USE_EXT_CALLBACKS
            pKnown->firstMessageReceived = true;
#endif
#ifdef KEEP_SESSION_NAME
            strncpy(pKnown->sessionName, invitation.sessionName, Settings::MaxSessionNameLen);
            pKnown->sessionName[Settings::MaxSessionNameLen] = '\\0';
            strncpy(invitation.sessionName, localName, Settings::MaxSessionNameLen);
            invitation.sessionName[Settings::MaxSessionNameLen] = '\\0';
#endif
            writeInvitation(controlPort, pKnown->remoteIP, pKnown->remotePort, invitation, amInvitationAccepted);
        }
        return;
    }
#else
    if (participant.ssrc == invitation.ssrc)
        return;
#endif
""",
    ),
    Patch(
        "name-nul",
        "AppleMIDI.hpp",
        """    participant.lastSyncExchangeTime = now;
#ifdef KEEP_SESSION_NAME
    strncpy(participant.sessionName, invitation.sessionName, Settings::MaxSessionNameLen);
#endif
""",
        """    participant.lastSyncExchangeTime = now;
#ifdef KEEP_SESSION_NAME
    strncpy(participant.sessionName, invitation.sessionName, Settings::MaxSessionNameLen);
    participant.sessionName[Settings::MaxSessionNameLen] = '\\0';  // [patched:name-nul v1]
#endif
""",
    ),
    Patch(
        "journal-length",
        "rtpMIDI_Parser_JournalSection.hpp",
        """            uint8_t chanjourlen = (chanflags & RTP_MIDI_CJ_MASK_LENGTH) >> 8;
""",
        """            uint16_t chanjourlen = (chanflags & RTP_MIDI_CJ_MASK_LENGTH) >> 8;  // [patched:journal-length v1] 10 bits
""",
    ),
    Patch(
        "journal-channels",
        "rtpMIDI_Parser_JournalSection.hpp",
        """        _journalTotalChannels--;
    }
""",
        """        _journalTotalChannels--;
        _channelJournalSectionComplete = false;  // [patched:journal-channels v1] read the next channel's header
    }
""",
    ),
    Patch(
        "datagram-reset",
        "rtpMIDI_Parser.h",
        """public:
	AppleMIDISession<UdpClass, Settings, Platform> * session;
""",
        """public:
	AppleMIDISession<UdpClass, Settings, Platform> * session;

    // [patched:datagram-reset v1] Forget any half-parsed packet: called at
    // every datagram boundary (AppleMIDISession::parseDataPackets).
    void reset()
    {
        _rtpHeadersComplete = false;
        _journalSectionComplete = false;
        _channelJournalSectionComplete = false;
        midiCommandLength = 0;
        _journalTotalChannels = 0;
        rtpMidi_Flags = 0;
        cmdCount = 0;
        runningstatus = 0;
        _bytesToFlush = 0;
    }
""",
    ),
    Patch(
        "datagram-control",
        "AppleMIDI.hpp",
        """void AppleMIDISession<UdpClass, Settings, Platform>::parseControlPackets()
{
    while (controlBuffer.size() > 0)
    {
        auto retVal = _appleMIDIParser.parse(controlBuffer, amPortType::Control);
        if (retVal == parserReturn::Processed
        ||  retVal == parserReturn::NotEnoughData
        ||  retVal == parserReturn::NotSureGiveMeMoreData)
        {
            break;
        }
        else if (retVal == parserReturn::UnexpectedData)
        {
#ifdef USE_EXT_CALLBACKS
            if (nullptr != _exceptionCallback)
                _exceptionCallback(ssrc, ParseException, 0);
#endif
            controlBuffer.pop_front();
        }
        else if (retVal == parserReturn::SessionNameVeryLong)
        {
            // purge the rest of the data in controlPort
            while (controlPort.read() >= 0) {}
        }
    }
}
""",
        """void AppleMIDISession<UdpClass, Settings, Platform>::parseControlPackets()
{
    // [patched:datagram-control v1] One datagram is one AppleMIDI command:
    // parse it once, and drop whatever it leaves (see parseDataPackets).
    if (controlBuffer.size() > 0)
    {
        auto retVal = _appleMIDIParser.parse(controlBuffer, amPortType::Control);
        if (retVal == parserReturn::UnexpectedData)
        {
#ifdef USE_EXT_CALLBACKS
            if (nullptr != _exceptionCallback)
                _exceptionCallback(ssrc, ParseException, 0);
#endif
        }
        else if (retVal == parserReturn::SessionNameVeryLong)
        {
            // purge the rest of the data in controlPort
            while (controlPort.read() >= 0) {}
        }
    }
    if (controlPort.available() == 0)
        controlBuffer.clear();
}
""",
    ),
    Patch(
        "datagram-data",
        "AppleMIDI.hpp",
        """void AppleMIDISession<UdpClass, Settings, Platform>::parseDataPackets()
{
    while (dataBuffer.size() > 0)
    {
        auto retVal1 = _rtpMIDIParser.parse(dataBuffer);
        if (retVal1 == parserReturn::Processed
        ||  retVal1 == parserReturn::NotEnoughData)
            break;

        auto retVal2 = _appleMIDIParser.parse(dataBuffer, amPortType::Data);
        if (retVal2 == parserReturn::Processed
        ||  retVal2 == parserReturn::NotEnoughData)
            break;

        //  // both don't have data to determine protocol
        if (retVal1 == parserReturn::NotSureGiveMeMoreData
        &&  retVal2 == parserReturn::NotSureGiveMeMoreData)
            break;

        // one or the other don't have enough data to determine the protocol
        if (retVal1 == parserReturn::NotSureGiveMeMoreData
        ||  retVal2 == parserReturn::NotSureGiveMeMoreData)
            break; // one or the other buffer does not have enough data

#ifdef USE_EXT_CALLBACKS
        if (nullptr != _exceptionCallback)
            _exceptionCallback(ssrc, UnexpectedParseException, 0);
#endif
         dataBuffer.pop_front();
    }
}
""",
        """void AppleMIDISession<UdpClass, Settings, Platform>::parseDataPackets()
{
    // [patched:datagram-data v1] UDP carries exactly one RTP packet or one
    // AppleMIDI command per datagram, so each datagram is parsed once, on its
    // own. The stock loop kept whatever a datagram left unparsed (a truncated
    // or malformed packet, trailing bytes) with the parser mid-packet, parsed
    // the NEXT datagram as its continuation, and resynchronised on garbage one
    // byte at a time -- so one bad datagram could swallow the ones after it.
    if (dataBuffer.size() > 0)
    {
        auto retVal1 = _rtpMIDIParser.parse(dataBuffer);
        if (retVal1 != parserReturn::Processed
        &&  retVal1 != parserReturn::NotEnoughData)
        {
            auto retVal2 = _appleMIDIParser.parse(dataBuffer, amPortType::Data);
            if (retVal2 != parserReturn::Processed
            &&  retVal2 != parserReturn::NotEnoughData
            &&  retVal2 != parserReturn::SessionNameVeryLong
            &&  retVal1 != parserReturn::NotSureGiveMeMoreData
            &&  retVal2 != parserReturn::NotSureGiveMeMoreData)
            {
#ifdef USE_EXT_CALLBACKS
                if (nullptr != _exceptionCallback)
                    _exceptionCallback(ssrc, UnexpectedParseException, 0);
#endif
            }
        }
    }
    // A datagram bigger than the buffer (impossible while MaxBufferSize
    // exceeds WiFiUDP's 1460-byte receive cap) keeps the stream behaviour
    // until its last byte is in.
    if (dataPort.available() == 0)
    {
        dataBuffer.clear();
        _rtpMIDIParser.reset();
    }
}
""",
    ),
    Patch(
        "seqwrap-rx",
        "AppleMIDI.hpp",
        """        else if (rtp.sequenceNr - pParticipant->receiveSequenceNr - 1 != 0) {
            if (nullptr != _exceptionCallback)
                _exceptionCallback(ssrc, ReceivedPacketsDropped, rtp.sequenceNr - pParticipant->receiveSequenceNr - 1);
        }
""",
        """        else if ((uint16_t)(rtp.sequenceNr - pParticipant->receiveSequenceNr - 1) != 0) {  // [patched:seqwrap-rx v1]
            if (nullptr != _exceptionCallback)
                _exceptionCallback(ssrc, ReceivedPacketsDropped, (int16_t)(rtp.sequenceNr - pParticipant->receiveSequenceNr - 1));
        }
""",
    ),
    Patch(
        "seqwrap-feedback",
        "AppleMIDI.hpp",
        """    if (pParticipant->sendSequenceNr < receiverFeedback.sequenceNr)
    {
#ifdef USE_EXT_CALLBACKS
        if (nullptr != _exceptionCallback)
            _exceptionCallback(pParticipant->ssrc, SendPacketsDropped, pParticipant->sendSequenceNr - receiverFeedback.sequenceNr);
#endif
    }
""",
        """    if ((int16_t)(pParticipant->sendSequenceNr - receiverFeedback.sequenceNr) < 0)  // [patched:seqwrap-feedback v1]
    {
#ifdef USE_EXT_CALLBACKS
        if (nullptr != _exceptionCallback)
            _exceptionCallback(pParticipant->ssrc, SendPacketsDropped, (int16_t)(pParticipant->sendSequenceNr - receiverFeedback.sequenceNr));
#endif
    }
""",
    ),
]


def block_regex(text):
    # Library lines carry stray trailing whitespace that editors strip from
    # this script: match every line with or without it.
    lines = text.split("\n")
    return re.compile(
        "\n".join(re.escape(line.rstrip()) + r"[ \t]*" for line in lines[:-1])
        + "\n"
        + re.escape(lines[-1])
    )


def fail(msg):
    sys.stderr.write("\npatch_applemidi.py: %s\n" % msg)
    env.Exit(1)  # noqa: F821


def apply_patch(src, patch, path):
    if len(block_regex(patch.new).findall(src)) == 1:
        return src, False  # already applied
    if patch.tag_prefix in src:
        fail(
            "%s carries a different version of patch '%s' -- a stale patched\n"
            "copy. Delete .pio/libdeps and rebuild." % (path, patch.name)
        )
    matches = list(block_regex(patch.old).finditer(src))
    for legacy in patch.legacy:
        matches += list(block_regex(legacy).finditer(src))
    if len(matches) != 1:
        fail(
            "patch '%s': expected exactly one match of its source block in\n"
            "  %s\nfound %d. The library changed -- re-check the patch before\n"
            "building against it." % (patch.name, path, len(matches))
        )
    m = matches[0]
    return src[: m.start()] + patch.new + src[m.end():], True


def main():
    if not os.path.isdir(LIB):
        # Not installed yet; rtp_midi.cpp's PATCHSET check stops the build
        # if it never gets patched.
        return
    changed = []
    by_file = {}
    for patch in PATCHES:
        by_file.setdefault(patch.filename, []).append(patch)
    for filename, patches in by_file.items():
        path = os.path.join(LIB, filename)
        with open(path, "r", encoding="utf-8", newline="") as fh:
            src = fh.read()
        orig = src
        for patch in patches:
            src, applied = apply_patch(src, patch, path)
            if applied:
                changed.append(patch.name)
        if src != orig:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(src)

    # Every patch is in place: stamp the patch-set version.
    path = os.path.join(LIB, STAMP_FILE)
    with open(path, "r", encoding="utf-8", newline="") as fh:
        src = fh.read()
    stamped = STAMP_RE.sub("", src)
    if not stamped.startswith("#pragma once\n"):
        fail("%s no longer starts with '#pragma once'" % path)
    stamped = "#pragma once\n" + STAMP + stamped[len("#pragma once\n"):]
    if stamped != src:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(stamped)
    if changed:
        print("patch_applemidi.py: applied %s" % ", ".join(changed))


main()
