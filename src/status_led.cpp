#include "status_led.h"

#include <Arduino.h>

namespace {
constexpr uint8_t LED_PIN = 48;
constexpr uint8_t BRIGHTNESS = 25;  // WS2812 is blinding at full power
constexpr uint32_t BLINK_INTERVAL_MS = 250;

LedStatus current = LedStatus::Boot;
bool blinkOn = true;
uint32_t lastBlink = 0;

void apply() {
    switch (current) {
        case LedStatus::Boot:
            neopixelWrite(LED_PIN, BRIGHTNESS, BRIGHTNESS, BRIGHTNESS);
            break;
        case LedStatus::WifiConnecting:
            neopixelWrite(LED_PIN, 0, 0, blinkOn ? BRIGHTNESS : 0);
            break;
        case LedStatus::WifiConnected:
            neopixelWrite(LED_PIN, 0, BRIGHTNESS, 0);
            break;
        case LedStatus::SessionActive:
            neopixelWrite(LED_PIN, 0, BRIGHTNESS, BRIGHTNESS);
            break;
        case LedStatus::Error:
            neopixelWrite(LED_PIN, BRIGHTNESS, 0, 0);
            break;
    }
}
}  // namespace

void StatusLed::begin() {
    apply();
}

void StatusLed::set(LedStatus status) {
    if (status == current) return;
    current = status;
    blinkOn = true;
    apply();
}

void StatusLed::tick() {
    if (current != LedStatus::WifiConnecting) return;
    uint32_t now = millis();
    if (now - lastBlink >= BLINK_INTERVAL_MS) {
        lastBlink = now;
        blinkOn = !blinkOn;
        apply();
    }
}
