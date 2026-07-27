#include <Arduino.h>

#include "boot_guard.h"
#include "config.h"
#include "rtp_midi.h"
#include "status_led.h"
#include "web_ui.h"
#include "wifi_net.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// mDNS hostname -> esp32-midi.local
static const char* HOSTNAME = "esp32-midi";

// Holding BOOT (GPIO0) for 10s wipes all settings back to defaults -- the
// recovery path for a forgotten web UI password on a headless device.
static constexpr uint8_t RESET_BTN_PIN = 0;
static constexpr uint32_t RESET_HOLD_MS = 10000;
static uint32_t resetHeldSince = 0;

static void factoryResetTick() {
    if (digitalRead(RESET_BTN_PIN) == LOW) {
        if (resetHeldSince == 0) {
            resetHeldSince = millis();
        } else if (millis() - resetHeldSince >= RESET_HOLD_MS) {
            Serial.println("[reset] BOOT held 10s -- wiping settings, rebooting");
            StatusLed::set(LedStatus::Error);
            Config::wipeAll();
            delay(500);
            ESP.restart();
        }
    } else {
        resetHeldSince = 0;
    }
}

void setup() {
    Serial.begin(115200);
    BootGuard::begin();
    delay(500);
    Serial.println();
    Serial.println("ESP32-MIDI-WIFI v" FW_VERSION);
    Serial.println("USB MIDI -> RTP-MIDI wireless bridge");

    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    StatusLed::begin();
    Config::load();
    WifiNet::begin(Config::get().wifiSsid.c_str(), Config::get().wifiPass.c_str(), HOSTNAME);
    WebUi::begin();
}

void loop() {
    StatusLed::tick();
    WifiNet::tick();

    // AppleMIDI needs live sockets, so the session starts on first connect.
    if (!RtpMidi::isStarted() && WifiNet::isConnected()) {
        RtpMidi::begin();
    }
    RtpMidi::tick();
    WebUi::tick();
    BootGuard::tick();
    factoryResetTick();

    delay(1);
}
