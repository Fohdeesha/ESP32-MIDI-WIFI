#include "wifi_net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "status_led.h"

namespace {
const char* s_hostname = "esp32-midi";
bool mdnsUp = false;

void startMdns() {
    if (mdnsUp) return;
    if (!MDNS.begin(s_hostname)) {
        Serial.println("[net] mDNS start failed");
        return;
    }
    MDNS.addService("apple-midi", "udp", 5004);
    mdnsUp = true;
    Serial.printf("[net] mDNS up: %s.local\n", s_hostname);
}

void onWifiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[net] connected, IP %s (RSSI %d dBm)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            StatusLed::set(LedStatus::WifiConnected);
            startMdns();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            StatusLed::set(LedStatus::WifiConnecting);
            break;
        default:
            break;
    }
}
}  // namespace

void WifiNet::begin(const char* ssid, const char* password, const char* hostname) {
    s_hostname = hostname;
    StatusLed::set(LedStatus::WifiConnecting);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.setSleep(false);  // modem sleep adds latency spikes — unacceptable for MIDI
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    WiFi.begin(ssid, password);
    // Full 20 dBm TX glitches the CH340 USB link on this board (RFI/current
    // spike with the external antenna attached); 11 dBm is plenty.
    WiFi.setTxPower(WIFI_POWER_11dBm);
    Serial.printf("[net] connecting to \"%s\"...\n", ssid);
}

void WifiNet::tick() {
    // Reconnection is handled by WiFi.setAutoReconnect; nothing to do yet.
}

bool WifiNet::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}
