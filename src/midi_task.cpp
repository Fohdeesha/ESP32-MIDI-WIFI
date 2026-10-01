#include "midi_task.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "buffered_udp.h"
#include "config.h"
#include "midi_bridge.h"
#include "recorder.h"
#include "usb_midi_host.h"
#include "wifi_net.h"

namespace {
constexpr uint32_t STACK_BYTES = 8192;
constexpr UBaseType_t PRIORITY = 4;
constexpr BaseType_t CORE = 1;
constexpr uint32_t SNAPSHOT_MS = 100;
// A pass whose notification was already pending did not block. After this
// many of those in a row the task blocks for a tick anyway, so a sustained
// flood can never keep it ready for good and starve loop() (whose watchdog
// would then reset the board). Core 1's idle task is not watched, so a
// spinning task would otherwise starve loop() without anything noticing.
constexpr int MAX_BUSY_PASSES = 50;
constexpr uint32_t BLOCKED_US = 50;  // a wait shorter than this did not block
constexpr uint32_t TASK_RECORD_US = 5000;  // passes longer go in the flight recorder

TaskHandle_t s_task = nullptr;

portMUX_TYPE s_snapMux = portMUX_INITIALIZER_UNLOCKED;
MidiTask::Snapshot s_snap = {};  // under s_snapMux
uint32_t s_published = 0;
uint32_t s_lastSnapMs = 0;

// Timing window, written by the task only. resetStats() just asks.
volatile bool s_resetReq = false;
uint32_t s_passMaxUs = 0;
uint64_t s_passSumUs = 0;
uint64_t s_passes = 0;  // at up to ~2 kHz, 32 bits would wrap within a month
uint32_t s_periodMaxUs = 0;
uint32_t s_wakesUsb = 0;
uint32_t s_wakesNet = 0;  // 1.9.1: datagrams wake the task too
uint32_t s_wakesTimer = 0;
uint32_t s_stackFree = 0;
uint32_t s_lastStackMs = 0;
// Per stage (1.9.1): which part of a pass a long one went to.
uint32_t s_rtpMaxUs = 0;
uint32_t s_bridgeMaxUs = 0;
uint32_t s_healthMaxUs = 0;
uint32_t s_rtpMsgsMax = 0;
uint32_t s_passMaxAtMs = 0;

void applyReset(uint32_t nowMs) {
    s_resetReq = false;
    s_passMaxUs = 0;
    s_passSumUs = 0;
    s_passes = 0;
    s_periodMaxUs = 0;
    s_wakesUsb = 0;
    s_wakesNet = 0;
    s_wakesTimer = 0;
    s_rtpMaxUs = s_bridgeMaxUs = s_healthMaxUs = 0;
    s_rtpMsgsMax = 0;
    s_passMaxAtMs = 0;
    s_lastSnapMs = nowMs - SNAPSHOT_MS;  // publish the zeroed window at once
}

// The high-water mark scans the stack, so only once a second, and outside the
// timed part of the pass.
void sampleStack(uint32_t nowMs) {
    if (s_stackFree && nowMs - s_lastStackMs < 1000) return;
    s_lastStackMs = nowMs;
    s_stackFree = uxTaskGetStackHighWaterMark(nullptr);
}

void publishSnapshot(uint32_t nowMs) {
    if (s_published && nowMs - s_lastSnapMs < SNAPSHOT_MS) return;
    s_lastSnapMs = nowMs;
    MidiTask::Snapshot s = {};
    s.published = ++s_published;
    s.peerCount = RtpMidi::peerCount();
    for (int i = 0; i < s.peerCount && i < RtpMidi::MAX_PEERS; i++) {
        strlcpy(s.peerNames[i], RtpMidi::peerName(i), sizeof(s.peerNames[i]));
    }
    s.forwarded = MidiBridge::forwardedCount();
    s.uplinkPackets = MidiBridge::uplinkPackets();
    s.returned = MidiBridge::returnedCount();
    s.usbEvents = UsbMidi::eventCount();
    s.txDelivered = UsbMidi::txPacketCount();
    s.txDropped = UsbMidi::txDropCount();
    for (uint8_t c = 0; c < 16; c++) s.cableRx[c] = UsbMidi::cableRxCount(c);
    s.passMaxUs = s_passMaxUs;
    s.passMeanUs = s_passes ? (uint32_t)(s_passSumUs / s_passes) : 0;
    s.periodMaxUs = s_periodMaxUs;
    s.wakesUsb = s_wakesUsb;
    s.wakesNet = s_wakesNet;
    s.wakesTimer = s_wakesTimer;
    s.stackFree = s_stackFree;
    s.rtpMaxUs = s_rtpMaxUs;
    s.bridgeMaxUs = s_bridgeMaxUs;
    s.healthMaxUs = s_healthMaxUs;
    s.rtpMsgsMax = s_rtpMsgsMax;
    s.passMaxAtMs = s_passMaxAtMs;
    portENTER_CRITICAL(&s_snapMux);
    s_snap = s;
    portEXIT_CRITICAL(&s_snapMux);
}

void run(void*) {
    // A hung MIDI task panics into a TASK_WDT reset, which BootGuard counts.
    esp_task_wdt_add(nullptr);
    sampleStack(millis());  // so the first snapshot already has a figure
    uint32_t lastStartUs = micros();
    int busy = 0;
    for (;;) {
        // Woken by USB input or (1.9.1) a datagram BufferedUDP's receive task
        // queued, else after at most one tick (1 ms), which still drives the
        // heartbeat, the invite cycle and a drain cut short by its budget.
        uint32_t woke = 0;
        // A drain cut short by its bound always rests a tick, so loop() runs
        // between slices of a long burst: while datagrams keep arriving their
        // notifications would otherwise skip every rest. They stay pending.
        if (busy >= MAX_BUSY_PASSES || RtpMidi::backlogged()) {
            vTaskDelay(1);
            busy = 0;
        } else {
            const uint32_t waitUs = micros();
            woke = ulTaskNotifyTake(pdTRUE, 1);
            busy = (woke && micros() - waitUs < BLOCKED_US) ? busy + 1 : 0;
        }
        // Who woke it: the receive task flags its notifications, so anything
        // else was USB. (If both did, it counts as network.)
        const bool netWake = woke && BufferedUDP::takeNotified();
        const bool usbWake = woke && !netWake;
        const uint32_t startUs = micros();
        esp_task_wdt_reset();
        if (s_resetReq) applyReset(millis());

        // AppleMIDI needs live sockets, so the session starts on first connect.
        if (!RtpMidi::isStarted() && WifiNet::isConnected()) RtpMidi::begin();
        const uint32_t msgs = (uint32_t)RtpMidi::tick();
        const uint32_t rtpEndUs = micros();
        MidiBridge::tick();
        const uint32_t bridgeEndUs = micros();
        MidiBridge::healthTick();
        const uint32_t healthEndUs = micros();
        publishSnapshot(millis());

        const uint32_t pass = micros() - startUs;
        const uint32_t period = startUs - lastStartUs;
        lastStartUs = startUs;
        if (pass > TASK_RECORD_US) {  // a long pass, lined up with what caused it
            uint8_t d[5];
            memcpy(d, &pass, 4);
            d[4] = (uint8_t)(msgs < 255 ? msgs : 255);
            Recorder::put(Recorder::TASK, 0, d, sizeof(d));
        }
        if (pass > s_passMaxUs) {
            s_passMaxUs = pass;
            s_passMaxAtMs = millis();
        }
        if (period > s_periodMaxUs) s_periodMaxUs = period;
        // Wall time, so a stage's figure includes any preemption by the USB
        // tasks (priority 5) while it ran -- which is part of what it costs.
        if (rtpEndUs - startUs > s_rtpMaxUs) s_rtpMaxUs = rtpEndUs - startUs;
        if (bridgeEndUs - rtpEndUs > s_bridgeMaxUs) s_bridgeMaxUs = bridgeEndUs - rtpEndUs;
        if (healthEndUs - bridgeEndUs > s_healthMaxUs) s_healthMaxUs = healthEndUs - bridgeEndUs;
        if (msgs > s_rtpMsgsMax) s_rtpMsgsMax = msgs;
        s_passSumUs += pass;
        s_passes++;
        if (usbWake) {
            s_wakesUsb++;
        } else if (netWake) {
            s_wakesNet++;
        } else {
            s_wakesTimer++;
        }
        sampleStack(millis());
    }
}
}  // namespace

void MidiTask::begin() {
    const Config::Values& c = Config::get();
    MidiBridge::begin(c.usbCable, c.usbCableOut);
    RtpMidi::configure(c.sessionName.c_str(), c.targetIp.c_str(), c.targetPort);
    if (xTaskCreatePinnedToCore(run, "midi", STACK_BYTES, nullptr, PRIORITY, &s_task, CORE) !=
        pdPASS) {
        s_task = nullptr;
        Serial.println("[midi] task create failed -- nothing will be bridged");
        return;
    }
    UsbMidi::setRxNotify(s_task);
    BufferedUDP::setNotify(s_task);
}

void MidiTask::snapshot(Snapshot& out) {
    portENTER_CRITICAL(&s_snapMux);
    out = s_snap;
    portEXIT_CRITICAL(&s_snapMux);
}

void MidiTask::resetStats() {
    s_resetReq = true;
}
