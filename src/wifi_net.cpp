#include "wifi_net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "status_led.h"

namespace {
const char* s_hostname = "esp32-midi";
bool mdnsUp = false;
bool apActive = false;
bool staticApplied = false;
uint32_t beginMs = 0;

// If station connect hasn't succeeded by then, open a setup AP so the web
// config UI stays reachable (e.g. after a bad SSID/password was saved).
constexpr uint32_t AP_FALLBACK_MS = 30000;
constexpr const char* AP_SSID = "ESP32-MIDI-Setup";
// WPA2 minimum is 8 chars; "midi" alone is rejected by softAP()
constexpr const char* AP_PASSWORD = "midimidi";

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

void startPortal() {
    if (apActive) return;
    apActive = true;
    WiFi.mode(WIFI_AP_STA);  // keep retrying the station side if configured
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    StatusLed::set(LedStatus::PortalActive);
    Serial.printf("[net] setup AP \"%s\" up at %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
}

// Applies the configured static IP before WiFi.begin(). Any invalid value
// falls back to DHCP (with a log) rather than bricking connectivity -- the
// web UI validates on save, so this only triggers on hand-edited NVS.
// Gateway and DNS are optional: blank gateway = 0.0.0.0 (isolated LAN),
// blank DNS = use the gateway.
bool applyStaticIp(const Config::Values& cfg) {
    if (!cfg.staticIp.length()) return false;  // DHCP
    IPAddress ip, mask, gw, dns;
    if (!ip.fromString(cfg.staticIp) || ip == IPAddress()) {
        Serial.println("[net] invalid static IP -- using DHCP");
        return false;
    }
    if (!mask.fromString(cfg.staticMask) || mask == IPAddress()) {
        Serial.println("[net] invalid static netmask -- using DHCP");
        return false;
    }
    if (cfg.staticGw.length() && !gw.fromString(cfg.staticGw)) {
        Serial.println("[net] invalid static gateway -- using DHCP");
        return false;
    }
    if (cfg.staticDns.length()) {
        if (!dns.fromString(cfg.staticDns)) {
            Serial.println("[net] invalid static DNS -- using DHCP");
            return false;
        }
    } else {
        dns = gw;
    }
    if (!WiFi.config(ip, gw, mask, dns)) {
        Serial.println("[net] WiFi.config() failed -- using DHCP");
        return false;
    }
    Serial.printf("[net] static IP %s mask %s gw %s dns %s\n",
                  ip.toString().c_str(), mask.toString().c_str(),
                  gw.toString().c_str(), dns.toString().c_str());
    return true;
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

void WifiNet::begin(const Config::Values& cfg, const char* hostname) {
    s_hostname = hostname;
    beginMs = millis();
    if (!cfg.wifiSsid.length()) {
        Serial.println("[net] no WiFi configured -- starting setup portal");
        startPortal();
        return;
    }
    StatusLed::set(LedStatus::WifiConnecting);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.setSleep(false);  // modem sleep adds latency spikes -- unacceptable for MIDI
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    staticApplied = applyStaticIp(cfg);  // must precede WiFi.begin()
    // Full 20 dBm TX glitches the CH340 USB link on this board (RFI/current
    // spike with the external antenna attached); 11 dBm is plenty -- measured
    // RSSI at the installed position is -39 dBm, so there is ample margin.
    // Set BEFORE begin() (1.5.4): some IDF versions re-apply the default power
    // during association, which would silently undo a post-begin() call.
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    Serial.printf("[net] connecting to \"%s\"...\n", cfg.wifiSsid.c_str());
}

void WifiNet::tick() {
    if (!apActive && WiFi.status() != WL_CONNECTED &&
        millis() - beginMs > AP_FALLBACK_MS) {
        Serial.println("[net] station connect timed out");
        startPortal();
    }
}

bool WifiNet::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiNet::usingStaticIp() {
    return staticApplied;
}
