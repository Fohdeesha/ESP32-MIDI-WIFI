#include "wifi_net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>

#include "status_led.h"

namespace {
const char* s_hostname = "esp32-midi";
bool mdnsUp = false;
bool apActive = false;
bool staticApplied = false;
uint32_t beginMs = 0;
String s_ssid;                  // for the reconnect kick (empty = unconfigured)

// ── WiFi health diagnostics (1.7.0) ─────────────────────────────────────────
// The pre-1.7.0 disconnect handler only changed the LED: no reason code, no
// counter, no history -- so a device that dropped off the network for minutes
// looked perfectly healthy once it was back. Everything here exists to make
// the NEXT dropout self-diagnosing from the status page / /diag.
struct WifiEv {
    uint32_t up_s;     // uptime when it happened
    uint8_t kind;      // 0 = disconnected, 1 = got IP, 2 = reconnect kick
    uint8_t reason;    // disconnect reason code (kind 0 only)
    int8_t rssi;       // last known RSSI before the event
};
constexpr uint8_t EV_RING = 16;
WifiEv s_evRing[EV_RING];
uint8_t s_evCount = 0;   // total recorded (ring keeps the newest EV_RING)
uint32_t s_discCount = 0;
uint32_t s_kickCount = 0;
uint8_t s_lastReason = 0;
int8_t s_lastRssi = 0;          // refreshed while connected (RSSI reads garbage
                                // once the link is gone, so keep the last good one)
uint32_t s_lastRssiMs = 0;
uint32_t s_lastKickMs = 0;
uint32_t s_disconnectedSinceMs = 0;  // 0 = not currently disconnected

void recordEv(uint8_t kind, uint8_t reason) {
    s_evRing[s_evCount % EV_RING] = {millis() / 1000, kind, reason, s_lastRssi};
    if (s_evCount < 0xFF) s_evCount++;
}

// Names for the disconnect reasons this class of dropout actually produces
// (esp_wifi_types.h). Anything else prints as a number.
const char* reasonName(uint8_t r) {
    switch (r) {
        case 2: return "AUTH_EXPIRE";
        case 3: return "AUTH_LEAVE";
        case 4: return "ASSOC_EXPIRE";
        case 5: return "ASSOC_TOOMANY";
        case 8: return "STA_LEAVING";
        case 15: return "4WAY_HANDSHAKE_TIMEOUT";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default: return nullptr;
    }
}

String reasonText(uint8_t r) {
    const char* n = reasonName(r);
    if (n) return String(n) + " (" + String(r) + ")";
    return String(r);
}

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

void onWifiEvent(WiFiEvent_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_lastRssi = WiFi.RSSI();
            s_disconnectedSinceMs = 0;
            recordEv(1, 0);
            Serial.printf("[net] connected, IP %s (RSSI %d dBm, TX %.1f dBm)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          Config::get().txPower / 4.0);
            if (apActive) {
                Serial.println("[net] station up, closing setup AP");
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                apActive = false;
            }
            StatusLed::set(LedStatus::WifiConnected);
            startMdns();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            // The reason code is the single most diagnostic byte a dropout
            // produces: BEACON_TIMEOUT = RF/interference/power, NO_AP_FOUND =
            // the AP vanished or changed channel, AUTH_/ASSOC_FAIL = AP-side
            // refusal. Log it, count it, remember it.
            const uint8_t r = info.wifi_sta_disconnected.reason;
            s_discCount++;
            s_lastReason = r;
            if (!s_disconnectedSinceMs) s_disconnectedSinceMs = millis() ? millis() : 1;
            recordEv(0, r);
            Serial.printf("[net] station DISCONNECTED, reason %s (disconnect #%lu, last RSSI %d dBm)\n",
                          reasonText(r).c_str(), (unsigned long)s_discCount, s_lastRssi);
            if (!apActive) StatusLed::set(LedStatus::WifiConnecting);
            break;
        }
        default:
            break;
    }
}
}  // namespace

void WifiNet::begin(const Config::Values& cfg, const char* hostname) {
    s_hostname = hostname;
    s_ssid = cfg.wifiSsid;
    beginMs = millis();
    Serial.printf("[net] reset reason: %s\n", WifiNet::resetReasonName());
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
    // TX power is config-driven since 1.7.0 (default 19.5 dBm, the maximum).
    // History: 1.5.x pinned 11 dBm because full power glitched the CH340
    // serial link at the bench -- but that only matters with the UART cabled,
    // and the deployed link margin turned out to need the power (RSSI at the
    // installed position degraded from -39 to -60 dBm over time). Lower it
    // from the web page if bench serial glitches return.
    // Set BEFORE begin() (1.5.4): some IDF versions re-apply the default power
    // during association, which would silently undo a post-begin() call.
    WiFi.setTxPower((wifi_power_t)cfg.txPower);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    Serial.printf("[net] connecting to \"%s\" (TX %.1f dBm)...\n",
                  cfg.wifiSsid.c_str(), cfg.txPower / 4.0);
}

void WifiNet::tick() {
    const uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        // Keep a last-known-good RSSI for the diagnostics: once the link is
        // gone, WiFi.RSSI() no longer means anything.
        if (now - s_lastRssiMs > 2000) {
            s_lastRssiMs = now;
            s_lastRssi = WiFi.RSSI();
        }
    } else if (s_ssid.length()) {
        // RECONNECT KICK (1.7.0): setAutoReconnect(true) is trusted to bring
        // the station back, but it retries on its own schedule and has been
        // observed wedged for minutes. If we have been disconnected for 15 s,
        // force a fresh association attempt ourselves, and keep forcing one
        // every 15 s until the link is back. Composes with the setup-AP
        // fallback below (AP_STA keeps retrying the station side).
        if (!s_disconnectedSinceMs) s_disconnectedSinceMs = now ? now : 1;
        if (now - s_disconnectedSinceMs > 15000 && now - s_lastKickMs > 15000) {
            s_lastKickMs = now;
            s_kickCount++;
            recordEv(2, 0);
            Serial.printf("[net] still disconnected -- forcing reconnect (kick #%lu)\n",
                          (unsigned long)s_kickCount);
            WiFi.reconnect();
        }
    }
    if (!apActive && WiFi.status() != WL_CONNECTED &&
        now - beginMs > AP_FALLBACK_MS) {
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

uint32_t WifiNet::disconnectCount() { return s_discCount; }
uint32_t WifiNet::reconnectKicks() { return s_kickCount; }

const char* WifiNet::resetReasonName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW_RESTART";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

void WifiNet::appendDiag(String& s) {
    s += "disconnects=";
    s += String(s_discCount);
    s += " kicks=";
    s += String(s_kickCount);
    s += " last_reason=";
    s += s_discCount ? reasonText(s_lastReason) : String("-");
    s += " reset=";
    s += resetReasonName();
    s += " txpwr_dbm=";
    s += String(Config::get().txPower / 4.0, 1);
}

String WifiNet::eventLog() {
    String out;
    const uint8_t n = s_evCount < EV_RING ? s_evCount : EV_RING;
    const uint8_t first = s_evCount < EV_RING ? 0 : s_evCount % EV_RING;
    for (uint8_t i = 0; i < n; i++) {
        const WifiEv& e = s_evRing[(first + i) % EV_RING];
        out += String(e.up_s) + "s ";
        if (e.kind == 1) {
            out += "connected";
        } else if (e.kind == 2) {
            out += "reconnect kick";
        } else {
            out += "DISCONNECTED reason " + reasonText(e.reason);
        }
        out += " (last RSSI " + String(e.rssi) + " dBm)\n";
    }
    return out;
}
