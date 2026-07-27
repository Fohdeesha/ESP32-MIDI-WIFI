#include <Arduino.h>

#include "rtp_midi.h"
#include "secrets.h"
#include "status_led.h"
#include "wifi_net.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// mDNS hostname -> esp32-midi.local
static const char* HOSTNAME = "esp32-midi";

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("ESP32-MIDI-WIFI v" FW_VERSION);
    Serial.println("USB MIDI -> RTP-MIDI wireless bridge");

    StatusLed::begin();
    WifiNet::begin(WIFI_SSID, WIFI_PASSWORD, HOSTNAME);
}

void loop() {
    StatusLed::tick();
    WifiNet::tick();

    // AppleMIDI needs live sockets, so the session starts on first connect.
    if (!RtpMidi::isStarted() && WifiNet::isConnected()) {
        RtpMidi::begin();
    }
    RtpMidi::tick();

    delay(1);
}
