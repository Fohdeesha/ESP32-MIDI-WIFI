#include "boot_guard.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>

namespace {
constexpr uint8_t MAX_FAILED_BOOTS = 3;
constexpr uint32_t STABLE_AFTER_MS = 30000;

Preferences prefs;
bool stable = false;

void setFailCount(uint8_t n) {
    prefs.begin("bootguard", false);
    prefs.putUChar("fails", n);
    prefs.end();
}
}  // namespace

void BootGuard::begin() {
    prefs.begin("bootguard", false);
    uint8_t fails = prefs.getUChar("fails", 0) + 1;
    prefs.putUChar("fails", fails);
    prefs.end();

    if (fails >= MAX_FAILED_BOOTS) {
        Serial.printf("[boot] %u consecutive failed boots\n", fails);
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
