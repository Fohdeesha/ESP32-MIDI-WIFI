#include "web_ui.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "boot_guard.h"
#include "config.h"
#include "rtp_midi.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

namespace {
WebServer server(80);
bool uploadAuthorized = false;

// "" = protection off. Basic auth, fixed username "admin". LAN-grade only.
bool authOk() {
    const String& p = Config::get().webPass;
    if (p.length() == 0) return true;
    return server.authenticate("admin", p.c_str());
}

const char PAGE_HEAD[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-MIDI-WIFI</title><style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:1em auto;padding:0 1em;background:#111;color:#ddd}
h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em;border-bottom:1px solid #333;padding-bottom:.3em}
table{border-collapse:collapse}td{padding:.15em .8em .15em 0;color:#aaa}td+td{color:#ddd}
label{display:block;margin:.7em 0 .2em;color:#aaa}
input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:.45em;background:#222;border:1px solid #444;border-radius:4px;color:#ddd}
button{margin-top:1em;padding:.5em 1.4em;background:#2a6;border:0;border-radius:4px;color:#fff;font-size:1em;cursor:pointer}
.warn{color:#fa5}small{color:#888}
</style></head><body><h1>ESP32-MIDI-WIFI</h1>
)html";

String statusSection() {
    String s = F("<h2>Status</h2><table>");
    s += "<tr><td>Firmware</td><td>v" FW_VERSION "</td></tr>";
    s += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    s += "<tr><td>RTP-MIDI peers</td><td>" + String(RtpMidi::peerCount()) + "</td></tr>";
    s += "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
    s += "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " kB</td></tr>";
    s += F("</table>");
    return s;
}

String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        switch (in[i]) {
            case '&': out += F("&amp;"); break;
            case '<': out += F("&lt;"); break;
            case '>': out += F("&gt;"); break;
            case '\'': out += F("&#39;"); break;
            case '"': out += F("&quot;"); break;
            default: out += in[i];
        }
    }
    return out;
}

// Serves the previous async scan's results and kicks off a fresh scan, so
// the list is at most one page-load stale. First-ever load shows none.
String ssidDatalist() {
    String opts;
    int n = WiFi.scanComplete();
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(i);
            if (!s.length()) continue;
            String esc = htmlEscape(s);
            if (opts.indexOf("'" + esc + "'") >= 0) continue;  // dedupe
            opts += "<option value='" + esc + "'>";
        }
        WiFi.scanDelete();
    }
    WiFi.scanNetworks(true);
    return "<datalist id='ssids'>" + opts + "</datalist>";
}

void handleRoot() {
    if (!authOk()) return server.requestAuthentication();
    const Config::Values& c = Config::get();
    String page = FPSTR(PAGE_HEAD);
    page += statusSection();
    page += F("<h2>Configuration</h2><form method='POST' action='/config'>"
              "<label>WiFi SSID <small>(pick from nearby networks or type; "
              "reload to refresh the list)</small></label>"
              "<input type='text' name='ssid' list='ssids' value='");
    page += htmlEscape(c.wifiSsid);
    page += F("'>");
    page += ssidDatalist();
    page += F("<label>WiFi password <small>(leave blank to keep current)</small></label>"
              "<input type='password' name='pass' value=''>"
              "<label>RTP-MIDI session name</label><input type='text' name='name' maxlength='24' value='");
    page += c.sessionName;
    page += F("'><label>Connect to peer (IP, blank = accept incoming only)</label>"
              "<input type='text' name='tip' value='");
    page += c.targetIp;
    page += F("'><label>Peer port</label><input type='number' name='tport' min='1' max='65535' value='");
    page += String(c.targetPort);
    page += F("'><label>Web UI password <small>(");
    page += c.webPass.length() ? F("set; blank = keep current") : F("not set; blank = stays off");
    page += F(")</small></label><input type='password' name='webpass' maxlength='63' value=''>"
              "<label><input type='checkbox' name='clearpass' value='1'> Remove web UI password</label>"
              "<button type='submit'>Save &amp; reboot</button></form>"
              "<h2>Firmware update</h2>"
              "<form method='POST' action='/update' enctype='multipart/form-data'>"
              "<input type='file' name='fw' accept='.bin'>"
              "<button type='submit'>Upload &amp; flash</button></form>"
              "<h2>Factory reset</h2>"
              "<form method='POST' action='/reset' "
              "onsubmit=\"return confirm('Erase all settings and reboot?')\">"
              "<button type='submit'>Reset to defaults</button></form>"
              "<p><small>Also available without the password: hold the BOOT button "
              "for 10 seconds.</small></p>"
              "<p><small>Device reboots after saving config, flashing firmware, or resetting.</small></p>"
              "</body></html>");
    server.send(200, "text/html", page);
}

void handleConfigPost() {
    if (!authOk()) return server.requestAuthentication();
    Config::Values v = Config::get();
    if (server.hasArg("ssid") && server.arg("ssid").length()) v.wifiSsid = server.arg("ssid");
    if (server.hasArg("pass") && server.arg("pass").length()) v.wifiPass = server.arg("pass");
    if (server.hasArg("name") && server.arg("name").length()) v.sessionName = server.arg("name");
    if (server.hasArg("tip")) v.targetIp = server.arg("tip");
    if (server.hasArg("tport")) {
        long p = server.arg("tport").toInt();
        if (p >= 1 && p <= 65535) v.targetPort = (uint16_t)p;
    }
    if (server.hasArg("clearpass") && server.arg("clearpass") == "1") {
        v.webPass = "";
    } else if (server.hasArg("webpass") && server.arg("webpass").length()) {
        v.webPass = server.arg("webpass");
    }
    bool ok = Config::save(v);
    server.send(ok ? 200 : 500, "text/html",
                ok ? "<meta http-equiv='refresh' content='8;url=/'>Saved. Rebooting..."
                   : "Failed to save config");
    if (ok) {
        BootGuard::markStable();
        delay(300);
        ESP.restart();
    }
}

void handleUpdatePost() {
    if (!authOk()) return server.requestAuthentication();
    if (!uploadAuthorized) {
        server.send(401, "text/plain", "unauthorized");
        return;
    }
    bool ok = !Update.hasError();
    server.send(ok ? 200 : 500, "text/html",
                ok ? "<meta http-equiv='refresh' content='12;url=/'>Flashed. Rebooting..."
                   : String("Update failed: ") + Update.errorString());
    if (ok) {
        BootGuard::markStable();
        delay(300);
        ESP.restart();
    }
}

void handleUpdateUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        // Gate the flash write itself, not just the completion response --
        // otherwise an unauthenticated POST would still reach the OTA slot.
        uploadAuthorized = authOk();
        if (!uploadAuthorized) {
            Serial.println("[web] unauthorized firmware upload rejected");
            return;
        }
        Serial.printf("[web] firmware upload start: %s\n", up.filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!uploadAuthorized) return;
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (!uploadAuthorized) return;
        if (Update.end(true)) {
            Serial.printf("[web] firmware upload done: %u bytes\n", up.totalSize);
        } else {
            Serial.printf("[web] firmware upload failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("[web] firmware upload aborted");
    }
}
}  // namespace

void handleResetPost() {
    if (!authOk()) return server.requestAuthentication();
    Serial.println("[web] factory reset requested");
    server.send(200, "text/html",
                "<meta http-equiv='refresh' content='10;url=/'>Settings erased. Rebooting...");
    Config::wipeAll();
    delay(300);
    ESP.restart();
}

void WebUi::begin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
    server.on("/reset", HTTP_POST, handleResetPost);
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    Serial.println("[web] config UI on http://esp32-midi.local/");
}

void WebUi::tick() {
    server.handleClient();
}
