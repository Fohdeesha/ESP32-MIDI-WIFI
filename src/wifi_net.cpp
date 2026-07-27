#include "wifi_net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "status_led.h"

namespace {
const char* s_hostname = "esp32-midi";
bool mdnsUp = false;
bool apActive = false;
uint32_t beginMs = 0;

// If station connect hasn't succeeded by then, open a setup AP so the web
// config UI stays reachable (e.g. after a bad SSID/password was saved).
constexpr uint32_t AP_FALLBACK_MS = 30000;
constexpr const char* AP_SSID = "ESP32-MIDI-Setup";

void startMdns() {
    if (mdnsUp) return;
    if (!MDNS.begin(s_hostname)) {
        Serial.println("[net] mDNS start failed");
        return;
    }
    MDNS.addService("apple-midi", "udp", 5004);
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
    Serial.printf("[net] mDNS up: %s.local\n", s_hostname);
}

void onWifiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[net] connected, IP %s (RSSI %d dBm)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            if (apActive) {
                Serial.println("[net] station up, closing setup AP");
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                apActive = false;
            }
            StatusLed::set(LedStatus::WifiConnected);
            startMdns();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (!apActive) StatusLed::set(LedStatus::WifiConnecting);
            break;
        default:
            break;
    }
}
}  // namespace

void WifiNet::begin(const char* ssid, const char* password, const char* hostname) {
    s_hostname = hostname;
    beginMs = millis();
    StatusLed::set(LedStatus::WifiConnecting);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.setSleep(false);  // modem sleep adds latency spikes -- unacceptable for MIDI
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    WiFi.begin(ssid, password);
    // Full 20 dBm TX glitches the CH340 USB link on this board (RFI/current
    // spike with the external antenna attached); 11 dBm is plenty.
    WiFi.setTxPower(WIFI_POWER_11dBm);
    Serial.printf("[net] connecting to \"%s\"...\n", ssid);
}

void WifiNet::tick() {
    if (!apActive && WiFi.status() != WL_CONNECTED &&
        millis() - beginMs > AP_FALLBACK_MS) {
        apActive = true;
        WiFi.mode(WIFI_AP_STA);  // keep retrying the station side
        WiFi.softAP(AP_SSID);
        StatusLed::set(LedStatus::PortalActive);
        Serial.printf("[net] station connect timed out; setup AP \"%s\" up at %s\n",
                      AP_SSID, WiFi.softAPIP().toString().c_str());
    }
}

bool WifiNet::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}
