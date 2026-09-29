#include <Arduino.h>

#include "boot_guard.h"
#include "config.h"
#include "log_queue.h"
#include "midi_task.h"
#include "rtp_midi.h"
#include "status_led.h"
#include "usb_midi_host.h"
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

// The status LED is decided here, in one place, from the modules' own state
// (1.7.2). It used to be set as a side effect of WiFi events (the WiFi event
// task) and session callbacks (the loop task), each overwriting the other: a
// session that survived a WiFi reconnect showed as "no session" until it ended.
static void ledTick() {
    LedStatus s;
    if (WifiNet::portalActive()) {
        s = LedStatus::PortalActive;
    } else if (!WifiNet::isConnected()) {
        s = LedStatus::WifiConnecting;
    } else if (RtpMidi::hasPeer()) {
        s = LedStatus::SessionActive;
    } else {
        s = LedStatus::WifiConnected;
    }
    StatusLed::set(s);
}

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
    LogQueue::begin();
    BootGuard::begin();
    delay(500);
    Serial.println();
    Serial.println("ESP32-MIDI-WIFI v" FW_VERSION);
    Serial.println("USB MIDI -> RTP-MIDI wireless bridge");

    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    StatusLed::begin();
    Config::load();
    WifiNet::begin(Config::get(), HOSTNAME);
    WebUi::begin();
    UsbMidi::begin(Config::get().usbIface);
    MidiTask::begin();
}

// Everything but MIDI (1.8.0). Forwarding runs in the MIDI task (midi_task.h)
// at a higher priority, so a slow pass here -- a page load, a slow client, an
// OTA upload, a burst of Serial output -- no longer delays MIDI. Until 1.8.0
// this loop pumped MIDI too, and every HTTP request stalled both directions.
void loop() {
    WifiNet::tick();
    ledTick();
    StatusLed::tick();
    WebUi::tick();
    BootGuard::tick();
    factoryResetTick();
    LogQueue::drain();

    delay(1);
}
