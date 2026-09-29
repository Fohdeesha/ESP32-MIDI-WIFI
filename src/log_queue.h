#pragma once

#include <cstdint>

// Serial output for tasks that must never block (1.8.0). Arduino-ESP32 gives
// HardwareSerial no TX ring, so a print blocks at wire rate once the 128-byte
// FIFO fills -- milliseconds per line at 115200, which is exactly the stall the
// MIDI path cannot afford (1.5.0). Lines go into a small queue instead and the
// loop task, which has nothing time-critical left to do, prints them. A full
// queue drops the line and counts it; nothing here ever waits.
namespace LogQueue {
// Call first thing in setup(); lines queued before it are counted as dropped.
void begin();
// Formats one line (no trailing newline needed) and queues it. Any task.
void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Prints everything queued so far. Loop task only.
void drain();
// Lines lost to a full queue since boot.
uint32_t drops();
}  // namespace LogQueue
