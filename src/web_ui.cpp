#include "web_ui.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "boot_guard.h"
#include "config.h"
#include "midi_bridge.h"
#include "rtp_midi.h"
#include "usb_midi_host.h"

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
.nets{margin:.5em 0;border:1px solid #333;border-radius:4px;padding:.2em .6em}
.nets summary{cursor:pointer;color:#8cf;padding:.3em 0}
.nets table{width:100%;border-collapse:collapse;margin:.2em 0 .4em}
.nets tr{cursor:pointer}
.nets tr:hover td{background:#222}
.nets td{padding:.35em .6em;border-bottom:1px solid #2a2a2a;color:#ddd}
.nets td.rssi{text-align:right;color:#888;white-space:nowrap;width:6em}
</style></head><body><h1>ESP32-MIDI-WIFI</h1>
)html";

String htmlEscape(const String& in);

String statusSection() {
    String s = F("<h2>Status</h2><table>");
    s += "<tr><td>Firmware</td><td>v" FW_VERSION "</td></tr>";
    s += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    s += "<tr><td>RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    String peers = String(RtpMidi::peerCount());
    if (Config::get().targetIp.length() && RtpMidi::peerCount() == 0) {
        peers += " (inviting " + htmlEscape(Config::get().targetIp) + ":" +
                 String(Config::get().targetPort) + ")";
    }
    s += "<tr><td>RTP-MIDI peers</td><td>" + peers + "</td></tr>";
    s += "<tr><td>USB MIDI</td><td>" + htmlEscape(UsbMidi::statusText()) + "</td></tr>";
    s += "<tr><td>USB events</td><td>" + String(UsbMidi::eventCount()) + "</td></tr>";
    s += "<tr><td>USB &rarr; RTP</td><td>" + String(MidiBridge::forwardedCount()) +
         " events (cable 1 only)</td></tr>";
    s += "<tr><td>RTP &rarr; USB</td><td>" + String(MidiBridge::returnedCount()) + " events, " +
         String(UsbMidi::txPacketCount()) + " packets delivered</td></tr>";
    s += "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
    s += "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " kB</td></tr>";
    s += F("</table>");
    if (UsbMidi::eventCount() > 0) {
        String ev;
        UsbMidi::appendRecentEvents(ev, "\n");
        s += F("<p><small>Recent MIDI events (reload to refresh):</small></p><pre>");
        s += htmlEscape(ev);
        s += F("</pre>");
    }
    if (UsbMidi::descriptorDump()[0]) {
        s += F("<details class='nets'><summary>USB descriptors</summary><pre>");
        s += htmlEscape(UsbMidi::descriptorDump());
        s += F("</pre></details>");
    }
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
// the list is at most one page-load stale. Rendered as clickable chips that
// fill the SSID input -- a <datalist> gets suppressed by browser password
// managers on forms that contain password fields.
String ssidChips() {
    String out;
    int n = WiFi.scanComplete();
    if (n > 0) {
        int order[64];
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {  // insertion sort by RSSI, strongest first
            int k = order[i], j = i - 1;
            while (j >= 0 && WiFi.RSSI(order[j]) < WiFi.RSSI(k)) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = k;
        }
        String rows;
        int count = 0;
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(order[i]);
            if (!s.length()) continue;
            String esc = htmlEscape(s);
            if (rows.indexOf("'>" + esc + "</td>") >= 0) continue;  // dedupe
            rows += "<tr onclick=\"document.forms[0].ssid.value=this.dataset.s\" data-s='" + esc +
                    "'><td class='net'>" + esc + "</td><td class='rssi'>" +
                    String(WiFi.RSSI(order[i])) + " dBm</td></tr>";
            count++;
        }
        out += F("<details class='nets'><summary>Available networks (");
        out += String(count);
        out += F(")</summary><table>");
        out += rows;
        out += F("</table></details>");
    } else if (n == 0) {
        out = F("<p><small>No networks found yet -- reload to rescan.</small></p>");
    } else {
        out = F("<p><small>Scanning for networks... reload in a few seconds.</small></p>");
    }
    if (n >= 0) WiFi.scanDelete();
    WiFi.scanNetworks(true);
    return out;
}

void handleRoot() {
    if (!authOk()) return server.requestAuthentication();
    const Config::Values& c = Config::get();
    String page = FPSTR(PAGE_HEAD);
    page += statusSection();
    page += F("<h2>Configuration</h2><form method='POST' action='/config' autocomplete='off'>"
              "<label>WiFi SSID <small>(type, or pick from available networks below)</small></label>"
              "<input type='text' name='ssid' autocomplete='off' value='");
    page += htmlEscape(c.wifiSsid);
    page += F("'>");
    page += ssidChips();
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
