#include "boot_guard.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace {
constexpr uint8_t MAX_FAILED_BOOTS = 3;
constexpr uint32_t STABLE_AFTER_MS = 30000;
// Task-watchdog timeout for the loop task, far beyond any legitimate pass: the
// longest are an OTA upload (fed per chunk, see feedWatchdog()) and a web
// request riding out its sockets' own 5 s timeouts.
constexpr uint32_t LOOP_WDT_S = 30;

Preferences prefs;
bool stable = false;
uint8_t s_fails = 0;  // the stored count, as last read or written

void setFailCount(uint8_t n) {
    if (n == s_fails) return;  // no NVS write when nothing changes
    prefs.begin("bootguard", false);
    prefs.putUChar("fails", n);
    prefs.end();
    s_fails = n;
}

// Did the previous run end in a crash? esp_reset_reason() says how it ended.
bool crashed(esp_reset_reason_t why) {
    switch (why) {
        case ESP_RST_PANIC:     // exception, abort(), failed assert
        case ESP_RST_INT_WDT:   // interrupt watchdog
        case ESP_RST_TASK_WDT:  // task watchdog
        case ESP_RST_WDT:       // any other watchdog
            return true;
        default:
            return false;
    }
}
}  // namespace

void BootGuard::begin() {
    prefs.begin("bootguard", false);
    s_fails = prefs.getUChar("fails", 0);
    prefs.end();
    // Only a run that ended in a crash counts toward the rollback (1.7.2).
    // Every boot used to, cleared only after 30 s of uptime -- so three quick
    // power cycles (a battery bank cutting out and back, a loose cable, a
    // brownout as WiFi starts) rolled the firmware back to the previous OTA
    // image: a silent downgrade without a single crash. A power-on, reset-pin,
    // brownout or software reset now neither counts nor clears the tally.
    if (crashed(esp_reset_reason())) {
        const uint8_t fails = s_fails < 0xFF ? s_fails + 1 : s_fails;
        setFailCount(fails);
        if (fails >= MAX_FAILED_BOOTS) {
            Serial.printf("[boot] %u consecutive crashed boots\n", fails);
            if (Update.canRollBack()) {
                Serial.println("[boot] rolling back to previous firmware");
                setFailCount(0);
                Serial.flush();
                Update.rollBack();
                ESP.restart();
            }
            Serial.println("[boot] no rollback image available, continuing");
            setFailCount(0);
        }
    }
    // A HANG must end in a counted reset as well, or an image that locks up
    // early -- no crash, no reset, and the web UI (OTA included) dead with it
    // -- would never be rolled back. Nothing watched core 1 in this build: the
    // task watchdog covers core 0's idle task only, and Arduino leaves the
    // loop task unsubscribed. Subscribe it; the loop task feeds it before
    // every loop() pass, and a pass stuck for LOOP_WDT_S panics into a
    // TASK_WDT reset -- counted above, and a headless bridge that hangs now
    // restarts itself instead of waiting for a power cycle. The loop task
    // shares core 1 with the USB client task (priority 5), so that task
    // spinning trips this too, by starving the loop.
    esp_task_wdt_init(LOOP_WDT_S, true);  // IDF 4.4: reconfigures the running TWDT
    enableLoopWDT();
}

void BootGuard::feedWatchdog() {
    esp_task_wdt_reset();
}

void BootGuard::tick() {
    if (!stable && millis() > STABLE_AFTER_MS) {
        markStable();
    }
}

void BootGuard::markStable() {
    if (stable) return;
    stable = true;
    setFailCount(0);
    Serial.println("[boot] marked stable");
}
