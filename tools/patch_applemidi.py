"""Strip lathoub/AppleMIDI 3.3.0's debug SysEx tracing at build time.

`rtpMIDI_Parser_CommandSection.hpp::decodeMidiSysEx()` prints every SysEx byte
to `Serial` with four unconditional `Serial.print` calls -- the file contains no
preprocessor directives at all, so this is not behind a debug switch. It is
left-over debug code in the only published release (3.3.0, Aug 2022; the
library has had no release since).

Why it matters here: Arduino-ESP32 leaves `HardwareSerial` with no TX ring
buffer (`_txBufferSize = 0`), so `uart_write_bytes` blocks at wire rate once
the 128-byte FIFO is full. Each SysEx byte therefore costs ~434 us of BLOCKING
UART time at 115200 baud ("0xAB " = 5 chars). A control surface's display
frames are SysEx, so a busy surface stalls the main loop for hundreds of
milliseconds per datagram:

  measured 2026-07-28 on a control-surface rig, before this patch --
    parseDataPackets(): max 206 ms for ONE 945-byte datagram, 37 ms average;
    loop() fell to ~4 Hz (236-722 ms per iteration), which delivered the
    USB->RTP fader stream in four clumps a second, lost 6% of outbound RTP
    packets to back-to-back bursts, and dropped inbound display writes.

Applied as a `pre:` extra_script so a clean CI checkout is patched too -- the
dependency stays declared and updatable rather than being vendored. Idempotent,
and FAILS THE BUILD if the expected text is absent, so a future release that
changes this code can never silently reintroduce the stall.
"""

import os
import sys

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

TARGET = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),  # noqa: F821
    env.subst("$PIOENV"),  # noqa: F821
    "AppleMIDI",
    "src",
    "rtpMIDI_Parser_CommandSection.hpp",
)

DEBUG_LINES = """        Serial.print("0x");
        Serial.print(octet < 16 ? "0" : "");
        Serial.print(octet, HEX);
        Serial.print(" ");
"""

MARKER = "// [patched] per-byte SysEx Serial tracing removed"


def main():
    if not os.path.isfile(TARGET):
        # Dependency not fetched yet on this pass; PlatformIO re-runs the
        # script after installing libraries.
        return
    with open(TARGET, "r", encoding="utf-8", newline="") as fh:
        src = fh.read()

    if MARKER in src:
        return  # already patched

    if DEBUG_LINES not in src:
        sys.stderr.write(
            "\npatch_applemidi.py: expected debug block not found in\n  %s\n"
            "The library changed. Re-check decodeMidiSysEx() for per-byte\n"
            "Serial output before removing this script.\n" % TARGET
        )
        env.Exit(1)  # noqa: F821

    src = src.replace(DEBUG_LINES, "        %s\n" % MARKER, 1)
    with open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(src)
    print("patch_applemidi.py: removed per-byte SysEx Serial tracing")


main()
