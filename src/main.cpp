#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("ESP32-MIDI-WIFI v" FW_VERSION);
    Serial.println("USB MIDI -> RTP-MIDI wireless bridge");
}

void loop() {
    delay(1000);
}
