#pragma once

#include <cstdint>

#include "rtp_midi.h"

// The MIDI task (1.8.0): one FreeRTOS task on core 1 at priority 4 that owns
// all MIDI forwarding. Until 1.8.0 it ran in loop() and took turns with the
// web server, so while an HTTP request was handled (a page build, a slow
// client, a whole OTA upload) nothing was forwarded in either direction. The
// web server stays in loop() at priority 1, below this task, so nothing it
// does can delay MIDI. The USB host tasks (priority 5) still preempt it: their
// callbacks are short, and an unpolled IN endpoint costs more than a late pass.
//
// Ownership rules for everything that crosses between this task and the
// others (in practice, almost always the web server):
//  1. Only the MIDI task touches AppleMIDI, MIDI, and any sending or
//     state-changing function in RtpMidi:: or MidiBridge::. Only it calls
//     UsbMidi::readPacket() and UsbMidi::writePacket().
//  2. Other tasks read MIDI state from snapshot(), never from live module
//     state. A 32-bit counter with a single writer may be read directly
//     (aligned 32-bit loads are atomic on this core); strings, rings and peer
//     names go through the snapshot or a lock.
//  3. Rings another task shows (the USB RX and TX rings, the session event
//     log) are formatted into a local buffer, then copied in under a portMUX
//     spinlock; readers copy out under the same lock. Never snprintf inside a
//     critical section.
//  4. The MIDI task never calls Config::get(). begin() copies what it needs
//     before the task starts, so Config::save() reassigning the live Strings
//     cannot race it.
//  5. No Serial in the MIDI task: a UART write blocks at wire rate (the 1.5.0
//     stall). Use LogQueue::printf(), which loop() drains.
//  6. Settings change only through save and reboot.
namespace MidiTask {
struct Snapshot {
    uint32_t published;  // snapshots so far; 0 = none yet
    int peerCount;
    char peerNames[RtpMidi::MAX_PEERS][RtpMidi::PEER_NAME_LEN + 1];
    uint32_t forwarded;      // USB -> RTP events
    uint32_t uplinkPackets;  // ...and the datagrams they went out in
    uint32_t returned;       // RTP -> USB events
    uint32_t usbEvents;      // MIDI events received from the device
    uint32_t txDelivered;    // packets the device accepted
    uint32_t txDropped;      // packets that never reached it
    uint32_t cableRx[16];    // events per virtual cable, since attach
    // Task timing since boot or the last resetStats().
    uint32_t passMaxUs;    // longest pass
    uint32_t passMeanUs;
    uint32_t periodMaxUs;  // longest gap from one pass start to the next
    uint32_t wakesUsb;     // passes started by USB input
    uint32_t wakesNet;     // ...by a datagram (1.9.1)
    uint32_t wakesTimer;   // ...by the 1 ms poll
    uint32_t stackFree;    // bytes, lowest since boot
    // Where the longest passes go (1.9.1): each stage's own worst time, the
    // most messages one RTP drain handled, and when the longest pass was.
    uint32_t rtpMaxUs;     // RtpMidi::tick (network -> USB, session upkeep)
    uint32_t bridgeMaxUs;  // MidiBridge::tick (USB -> network)
    uint32_t healthMaxUs;  // MidiBridge::healthTick (heartbeat, markers)
    uint32_t rtpMsgsMax;
    uint32_t passMaxAtMs;  // uptime of the longest pass
};

// Copies the settings the task needs and starts it. Call once, at the end of
// setup(), after UsbMidi::begin().
void begin();
// The latest snapshot, published by the task at most every 100 ms. Any task.
void snapshot(Snapshot& out);
// Restarts the timing window (/diagreset). The task applies it itself.
void resetStats();
}  // namespace MidiTask
