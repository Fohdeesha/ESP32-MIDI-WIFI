#include "midi_task.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "midi_bridge.h"
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
uint32_t s_wakesTimer = 0;
uint32_t s_stackFree = 0;
uint32_t s_lastStackMs = 0;

void applyReset(uint32_t nowMs) {
    s_resetReq = false;
    s_passMaxUs = 0;
    s_passSumUs = 0;
    s_passes = 0;
    s_periodMaxUs = 0;
    s_wakesUsb = 0;
    s_wakesTimer = 0;
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
    s.wakesTimer = s_wakesTimer;
    s.stackFree = s_stackFree;
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
        // Woken by USB input, else after at most one tick (1 ms): network
        // input is polled, since AppleMIDI keeps its sockets to itself.
        uint32_t woke = 0;
        if (busy >= MAX_BUSY_PASSES) {
            vTaskDelay(1);
            busy = 0;
        } else {
            const uint32_t waitUs = micros();
            woke = ulTaskNotifyTake(pdTRUE, 1);
            busy = (woke && micros() - waitUs < BLOCKED_US) ? busy + 1 : 0;
        }
        const uint32_t startUs = micros();
        esp_task_wdt_reset();
        if (s_resetReq) applyReset(millis());

        // AppleMIDI needs live sockets, so the session starts on first connect.
        if (!RtpMidi::isStarted() && WifiNet::isConnected()) RtpMidi::begin();
        RtpMidi::tick();
        MidiBridge::tick();
        MidiBridge::healthTick();
        publishSnapshot(millis());

        const uint32_t pass = micros() - startUs;
        const uint32_t period = startUs - lastStartUs;
        lastStartUs = startUs;
        if (pass > s_passMaxUs) s_passMaxUs = pass;
        if (period > s_periodMaxUs) s_periodMaxUs = period;
        s_passSumUs += pass;
        s_passes++;
        if (woke) {
            s_wakesUsb++;
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
}

void MidiTask::snapshot(Snapshot& out) {
    portENTER_CRITICAL(&s_snapMux);
    out = s_snap;
    portEXIT_CRITICAL(&s_snapMux);
}

void MidiTask::resetStats() {
    s_resetReq = true;
}
