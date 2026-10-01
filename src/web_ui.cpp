#include "web_ui.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "boot_guard.h"
#include "config.h"
#include "log_queue.h"
#include "midi_bridge.h"
#include "midi_task.h"
#include "rtp_midi.h"
#include "usb_midi_host.h"
// Generated from web/index.html by tools/embed_web.py, under the build dir.
#include "web_page.h"
#include "wifi_net.h"

// Runs in the loop task, below the MIDI task: MIDI state comes from
// MidiTask::snapshot() or a module's locked copy, per midi_task.h.
//
// The page itself is static (web/index.html, served from flash) and fills
// itself from small JSON documents (1.9.0). Until then the device built ~14 kB
// of HTML per load with String appends; now a load is a flash-to-socket copy,
// or a 304 once the browser has it, and the status JSON is ~1.5 kB formatted
// into one static buffer.

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

namespace {
WebServer server(80);
bool uploadAuthorized = false;
bool uploadEmpty = false;  // the last upload carried no file at all

// "" = protection off. Basic auth, fixed username "admin". LAN-grade only.
bool authOk() {
    const String& p = Config::get().webPass;
    if (p.length() == 0) return true;
    return server.authenticate("admin", p.c_str());
}

// Valid dotted-quad IPv4, e.g. "192.168.1.81". IPAddress::fromString alone
// is too lax for validation feedback (it accepts some malformed input on
// older cores), so require exactly four in-range decimal octets.
bool validIpv4(const String& s) {
    int octet = 0, digits = 0, dots = 0;
    for (size_t i = 0; i < s.length(); i++) {
        char ch = s[i];
        if (ch == '.') {
            if (digits == 0 || ++dots > 3) return false;
            octet = 0;
            digits = 0;
        } else if (ch >= '0' && ch <= '9') {
            octet = octet * 10 + (ch - '0');
            if (++digits > 3 || octet > 255) return false;
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0;
}

bool allDigits(const String& s) {
    if (!s.length()) return false;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

uint32_t ipv4Value(const String& s) {
    IPAddress a;
    a.fromString(s);
    return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
}

// A subnet mask is a run of ones followed only by zeros (255.0.255.0 is not
// one, and lwIP would route by it anyway), and leaves room for at least two
// hosts: under /31 or /32 nothing -- not even a gateway -- is on-link, so the
// device would join WiFi (no setup-AP fallback) and be unreachable.
bool validMask(const String& s) {
    const uint32_t m = ipv4Value(s);
    return m != 0 && (~m & (~m + 1)) == 0 && ~m >= 3;
}

// An address a host can actually use inside its subnet: not the subnet's own
// network or broadcast address, and not 0/8, loopback or multicast/reserved.
bool validHostAddr(uint32_t ip, uint32_t mask) {
    const uint32_t host = ip & ~mask;
    const uint8_t first = ip >> 24;
    return host != 0 && host != ~mask && first != 0 && first != 127 && first < 224;
}

// Refuses a state-changing POST that a page on ANOTHER origin made the
// browser send (1.7.2). Basic auth alone cannot stop that: the browser
// attaches cached credentials to any site's form POST, so any web page the
// user visits could reset, reflash or reconfigure the bridge. Browsers
// mark such requests with an Origin header naming the other site; this
// page's own forms send an Origin that matches the Host they were loaded
// from, and curl (the documented OTA route) sends none at all.
bool sameOrigin() {
    if (!server.hasHeader("Origin")) return true;
    String origin = server.header("Origin");
    const int scheme = origin.indexOf("://");
    if (scheme < 0) return false;  // includes "null" (sandboxed/file pages)
    origin = origin.substring(scheme + 3);
    return origin.equalsIgnoreCase(server.hostHeader());
}

// The names this device is actually reached by: an IP literal, or its own mDNS
// name. DNS rebinding points an attacker's domain at the device instead --
// the attacker's page and the request then share that origin, so the Origin
// check above passes, and with the documented default password (or none) the
// page could reflash the bridge. Its Host header still names the attacker's
// domain, which is refused here.
bool hostOk() {
    String host = server.hostHeader();
    if (host.length() && host[0] == '[') return true;  // IPv6 literal
    const int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    if (host.endsWith(".")) host = host.substring(0, host.length() - 1);
    if (validIpv4(host)) return true;
    const String name = WifiNet::hostname();
    return host.equalsIgnoreCase(name) || host.equalsIgnoreCase(name + ".local");
}

bool refuseCrossOrigin() {
    if (!sameOrigin()) {
        server.send(403, "text/plain", "Refused: request came from another site.");
        return true;
    }
    if (!hostOk()) {
        server.send(403, "text/plain",
                    String("Refused: to change settings, open this page as http://") +
                        WifiNet::hostname() + ".local/ or by the device's IP address.");
        return true;
    }
    return false;
}

// Gate for the /api/* reads: auth, exactly as the page itself had until 1.9.0.
// No Host check here, deliberately: the old page showed status and settings
// at whatever name the device was reached by (a router's DNS name included),
// and these carry no secret -- passwords only ever appear as "set or not".
// Everything that changes state still needs an IP or the .local name
// (refuseCrossOrigin).
bool apiAllowed() {
    if (!authOk()) {
        server.requestAuthentication();
        return false;
    }
    // Every /api/* answer is live data: never cached, never sniffed as HTML.
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    return true;
}

// Strict UTF-8, as a browser decodes it (no overlong forms, no surrogates).
// The page is UTF-8; a stored name in some other encoding -- typed into a
// pre-1.9.0 page, which declared no charset, or a router's legacy-encoded
// SSID -- would reach it as U+FFFD and be saved back that way.
bool validUtf8(const char* s) {
    const uint8_t* p = (const uint8_t*)s;
    while (*p) {
        const uint8_t c = *p;
        if (c < 0x80) {
            p++;
            continue;
        }
        int n;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;  // also stops at the terminator
            cp = cp << 6 | (p[i] & 0x3F);
        }
        const uint32_t min = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        p += n + 1;
    }
    return true;
}

void appendHex(String& out, const String& s) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t b = (uint8_t)s[i];
        out += HEX_DIGITS[b >> 4];
        out += HEX_DIGITS[b & 0x0F];
    }
}

// The exact bytes of an SSID sent as hex (see handleConfigPost). False on
// anything but 1-32 bytes of hex with no NUL.
bool ssidFromHex(const String& hex, String& out) {
    if (hex.length() < 2 || hex.length() > 64 || hex.length() % 2) return false;
    String s;
    for (size_t i = 0; i < hex.length(); i += 2) {
        int v = 0;
        for (size_t k = i; k < i + 2; k++) {
            const char ch = hex[k];
            const int d = ch >= '0' && ch <= '9'   ? ch - '0'
                          : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                          : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                   : -1;
            if (d < 0) return false;
            v = v << 4 | d;
        }
        if (v == 0) return false;
        s += (char)v;
    }
    out = s;
    return true;
}

// One JSON document built into a fixed buffer, with commas placed
// automatically. A document that does not fit is reported (ok() == false),
// never sent cut short. Strings are escaped for JSON and additionally for
// HTML (<, >, &), so even a mis-sniffed response could not carry markup.
class Json {
public:
    Json(char* buf, size_t cap) : b_(buf), cap_(cap) { b_[0] = '\0'; }
    void open() { sep(); put('{'); comma_ = false; }
    void close() { put('}'); comma_ = true; }
    void openArr() { sep(); put('['); comma_ = false; }
    void closeArr() { put(']'); comma_ = true; }
    void key(const char* k) { sep(); quoted(k); put(':'); comma_ = false; }
    void str(const char* s) { sep(); quoted(s); comma_ = true; }
    void str(const String& s) { str(s.c_str()); }
    void num(long v) {
        char t[16];
        snprintf(t, sizeof(t), "%ld", v);
        sep();
        raw(t);
        comma_ = true;
    }
    void unum(uint32_t v) {
        char t[16];
        snprintf(t, sizeof(t), "%lu", (unsigned long)v);
        sep();
        raw(t);
        comma_ = true;
    }
    void boolean(bool v) { sep(); raw(v ? "true" : "false"); comma_ = true; }
    bool ok() const { return !full_; }
    size_t length() const { return n_; }
    size_t room() const { return cap_ - n_; }
    const char* c_str() const { return b_; }

private:
    void put(char c) {
        if (n_ + 1 < cap_) {
            b_[n_++] = c;
            b_[n_] = '\0';
        } else {
            full_ = true;
        }
    }
    void raw(const char* s) {
        while (*s) put(*s++);
    }
    void sep() {
        if (comma_) put(',');
    }
    void quoted(const char* s) {
        put('"');
        for (; *s; s++) {
            const uint8_t c = (uint8_t)*s;
            if (c == '"' || c == '\\') {
                put('\\');
                put((char)c);
            } else if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '&') {
                char t[8];
                snprintf(t, sizeof(t), "\\u%04x", c);
                raw(t);
            } else {
                // Bytes >= 0x80 pass as they are. A name that is not valid
                // UTF-8 decodes to U+FFFD in the browser; the JSON stays valid.
                put((char)c);
            }
        }
        put('"');
    }
    char* b_;
    size_t cap_;
    size_t n_ = 0;
    bool comma_ = false;
    bool full_ = false;
};

// Shared by every JSON handler: they all run in the loop task, one request at
// a time. Static rather than on that task's 8 kB stack.
char s_json[6144];

void sendJson(const Json& j) {
    if (!j.ok()) {
        server.send(500, "text/plain", "response too large");
        return;
    }
    // send_P takes a pointer and a length: on this chip that is any memory,
    // and it spares a String copy of the whole document.
    server.send_P(200, "application/json", j.c_str(), j.length());
}

// Cables are numbered 0-15 on the wire but every host UI labels the same
// things "port 1..16", so the page shows both and never just one.
String portLabel(uint8_t cable) {
    return "port " + String(cable + 1) + " (cable " + String(cable) + ")";
}

String bridgedPortText() {
    uint8_t in = MidiBridge::bridgedCable();
    String s = in == Config::CABLE_ALL ? String("all ports merged") : portLabel(in);
    return s + " in, " + portLabel(MidiBridge::outputCable()) + " out";
}

// Flags a selection pointing at a cable the attached device doesn't declare in
// that direction -- the asymmetric case (a device's in and out cable counts are
// independent) that would otherwise be silently dead in one direction.
String portWarning() {
    UsbMidi::IfaceInfo f;
    if (!UsbMidi::claimedInterfaceInfo(f)) return String();
    String w;
    uint8_t in = MidiBridge::bridgedCable();
    uint8_t out = MidiBridge::outputCable();
    if (f.inCables && in != Config::CABLE_ALL && in >= f.inCables) {
        w += "The device declares only " + String(f.inCables) +
             " port(s) toward the network, so nothing will arrive on " + portLabel(in) + ". ";
    }
    if (f.outCables && out >= f.outCables) {
        w += "The device declares only " + String(f.outCables) +
             " port(s) from the network, so anything sent to " + portLabel(out) +
             " will be ignored. ";
    }
    return w;
}

void formatMac(char* out, size_t len, esp_mac_type_t type) {
    uint8_t m[6] = {};
    esp_read_mac(m, type);
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// The station MAC is what the router sees (DHCP reservations, client lists).
// While the setup AP is up, phones see the AP's own, one higher.
String macText() {
    char sta[18], ap[18];
    formatMac(sta, sizeof(sta), ESP_MAC_WIFI_STA);
    String s(sta);
    if (WifiNet::portalActive()) {
        formatMac(ap, sizeof(ap), ESP_MAC_WIFI_SOFTAP);
        s += " (setup AP ";
        s += ap;
        s += ")";
    }
    return s;
}

// The page: static, gzipped, from flash. no-cache makes the browser revalidate
// on every load, and the ETag (a hash of the page) turns that into a 304, so a
// reload costs a few hundred bytes of airtime instead of the whole page.
void handleRoot() {
    if (!authOk()) return server.requestAuthentication();
    const bool gz = server.header("Accept-Encoding").indexOf("gzip") >= 0;
    const char* etag = gz ? WEB_PAGE_ETAG_GZ : WEB_PAGE_ETAG_RAW;
    server.sendHeader("Cache-Control", "private, no-cache");
    server.sendHeader("ETag", etag);
    server.sendHeader("Vary", "Accept-Encoding");
    if (server.header("If-None-Match").indexOf(etag) >= 0) {
        server.send(304, "text/html", "");
        return;
    }
    if (gz) {
        server.sendHeader("Content-Encoding", "gzip");
        server.send_P(200, "text/html; charset=utf-8", (PGM_P)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
    } else {
        server.send_P(200, "text/html; charset=utf-8", (PGM_P)WEB_PAGE_RAW, WEB_PAGE_RAW_LEN);
    }
}

// The status table, as the text each row shows, plus a version per log so the
// page fetches a log only while it is open and only when it changed (0 =
// empty, and the page hides it). The page polls this every 2 s while it is
// visible: ~1.5 kB a time. That used to be ruled out because a page load
// stalled MIDI; since 1.8.0 the MIDI task preempts this, and what is left is
// airtime, about one more small TCP exchange than the heartbeat alone sends.
void handleApiStatus() {
    if (!apiAllowed()) return;
    MidiTask::Snapshot snap;
    MidiTask::snapshot(snap);
    const Config::Values& c = Config::get();
    char t[160];
    Json j(s_json, sizeof(s_json));
    j.open();
    j.key("rows");
    j.open();
    j.key("fw");
    j.str("v" FW_VERSION);
    j.key("ip");
    j.str(WiFi.localIP().toString() + (WifiNet::usingStaticIp() ? " (static)" : " (DHCP)"));
    j.key("mac");
    j.str(macText());
    snprintf(t, sizeof(t), "%d dBm (TX %.1f dBm)", (int)WiFi.RSSI(), c.txPower / 4.0);
    j.key("rssi");
    j.str(t);
    {
        // WiFi health (1.7.0): a dropout used to be invisible here -- the page
        // read "good RSSI" while the device had been off the network for
        // minutes. Disconnect count + last reason + reset reason answer the
        // "why did it drop?" question from the device itself.
        String w;
        WifiNet::appendDiag(w);
        j.key("wifi");
        j.str(w);
    }
    {
        String peers = String(snap.peerCount);
        if (c.targetIp.length() && snap.peerCount == 0) {
            peers += " (inviting " + c.targetIp + ":" + String(c.targetPort) + ")";
        }
        const int named = snap.peerCount < RtpMidi::MAX_PEERS ? snap.peerCount : RtpMidi::MAX_PEERS;
        for (int i = 0; i < named; i++) {
            peers += i ? ", " : " (";
            peers += snap.peerNames[i];
        }
        if (named > 0) peers += ")";
        j.key("peers");
        j.str(peers);
    }
    j.key("usb");
    j.str(UsbMidi::statusText());
    {
        // What is electrically on the port, whatever the stack made of it
        // (1.7.1): separates "the device isn't there" from "the device is
        // there but failed to enumerate", which used to read the same.
        String p;
        UsbMidi::appendPortDiag(p);
        j.key("port");
        j.str(p);
    }
    j.key("bridged");
    j.str(bridgedPortText());
    j.key("events");
    j.unum(snap.usbEvents);
    snprintf(t, sizeof(t), "%lu events in %lu packets", (unsigned long)snap.forwarded,
             (unsigned long)snap.uplinkPackets);
    j.key("up");
    j.str(t);
    snprintf(t, sizeof(t), "%lu events, %lu packets delivered, %lu dropped",
             (unsigned long)snap.returned, (unsigned long)snap.txDelivered,
             (unsigned long)snap.txDropped);
    j.key("down");
    j.str(t);
    {
        String d;
        UsbMidi::appendRxDiag(d);
        j.key("in");
        j.str(d);
    }
    {
        String d;
        UsbMidi::appendTxDiag(d);
        j.key("out");
        j.str(d);
    }
    // MIDI task timing (1.8.0): pass = time spent forwarding per wake-up,
    // period = longest gap between wake-ups, which is the added latency.
    snprintf(t, sizeof(t), "pass max %lu us, mean %lu us, period max %lu us, stack free %lu B",
             (unsigned long)snap.passMaxUs, (unsigned long)snap.passMeanUs,
             (unsigned long)snap.periodMaxUs, (unsigned long)snap.stackFree);
    j.key("task");
    j.str(t);
    snprintf(t, sizeof(t), "%lu s", (unsigned long)(millis() / 1000));
    j.key("uptime");
    j.str(t);
    snprintf(t, sizeof(t), "%lu kB (lowest %lu kB)", (unsigned long)(ESP.getFreeHeap() / 1024),
             (unsigned long)(ESP.getMinFreeHeap() / 1024));
    j.key("heap");
    j.str(t);
    j.close();
    // The logs are long and only wanted when something is being diagnosed,
    // so they stay collapsed under the table and load when opened.
    j.key("logs");
    j.open();
    j.key("usb_in");
    j.unum(snap.usbEvents);
    j.key("usb_out");
    j.unum(UsbMidi::txFormattedCount());
    j.key("session");
    j.unum(RtpMidi::eventLogVersion());
    j.key("wifi");
    j.unum(WifiNet::eventCount());
    j.key("stack");
    j.unum(UsbMidi::stackLogCount());
    j.key("desc");
    j.unum(UsbMidi::descriptorVersion());
    j.close();
    j.close();
    sendJson(j);
}

// The settings as stored, minus the secrets (the passwords are only "set or
// not"), plus what the port selects need: every MIDIStreaming interface on the
// attached device, the claimed one's declared cable counts per direction, and
// the traffic actually seen per cable -- descriptors are not always honest.
void handleApiConfig() {
    if (!apiAllowed()) return;
    const Config::Values& c = Config::get();
    MidiTask::Snapshot snap;
    MidiTask::snapshot(snap);
    Json j(s_json, sizeof(s_json));
    j.open();
    j.key("ssid");
    j.str(c.wifiSsid);
    j.key("name");
    j.str(c.sessionName);
    j.key("tip");
    j.str(c.targetIp);
    j.key("tport");
    j.unum(c.targetPort);
    j.key("sip");
    j.str(c.staticIp);
    j.key("smask");
    j.str(c.staticMask);
    j.key("sgw");
    j.str(c.staticGw);
    j.key("sdns");
    j.str(c.staticDns);
    j.key("uif");
    j.unum(c.usbIface);
    j.key("ucab");
    j.unum(c.usbCable);
    j.key("ucabo");
    j.unum(c.usbCableOut);
    j.key("txp");
    j.unum(c.txPower);
    j.key("txChoices");
    j.openArr();
    for (uint8_t choice : Config::TX_POWER_CHOICES) j.unum(choice);
    j.closeArr();
    j.key("txDefault");
    j.unum(Config::TX_POWER_DEFAULT);
    j.key("webPassSet");
    j.boolean(c.webPass.length() > 0);
    // A name that is not valid UTF-8 cannot round-trip through the page; it
    // leaves that field blank instead, and a blank field keeps what is stored.
    j.key("ssidOk");
    j.boolean(validUtf8(c.wifiSsid.c_str()));
    j.key("nameOk");
    j.boolean(validUtf8(c.sessionName.c_str()));
    // [bInterfaceNumber, has IN, has OUT, IN cables, OUT cables], one row per
    // alternate setting; the page collapses them.
    j.key("ifaces");
    j.openArr();
    UsbMidi::IfaceInfo f;
    for (uint8_t i = 0; i < UsbMidi::ifaceCount(); i++) {
        if (!UsbMidi::ifaceAt(i, f)) continue;
        j.openArr();
        j.unum(f.num);
        j.unum(f.epIn ? 1 : 0);
        j.unum(f.epOut ? 1 : 0);
        j.unum(f.inCables);
        j.unum(f.outCables);
        j.closeArr();
    }
    j.closeArr();
    const bool claimed = UsbMidi::claimedInterfaceInfo(f);
    j.key("declIn");
    j.unum(claimed ? f.inCables : 0);
    j.key("declOut");
    j.unum(claimed ? f.outCables : 0);
    j.key("cableRx");
    j.openArr();
    for (uint8_t n = 0; n < 16; n++) j.unum(snap.cableRx[n]);
    j.closeArr();
    j.key("bridging");
    j.str(bridgedPortText());
    j.key("warning");
    j.str(portWarning());
    j.close();
    sendJson(j);
}

// One diagnostic log as plain text, fetched when the page opens it.
void handleApiLog() {
    if (!apiAllowed()) return;
    const String name = server.arg("name");
    String out;
    if (name == "usb_in") {
        UsbMidi::appendRecentEvents(out, "\n");
    } else if (name == "usb_out") {
        UsbMidi::appendRecentTxEvents(out, "\n");
    } else if (name == "session") {
        RtpMidi::appendEventLog(out, "\n");
    } else if (name == "wifi") {
        out = WifiNet::eventLog();
    } else if (name == "stack") {
        UsbMidi::appendStackLog(out, "\n");
    } else if (name == "desc") {
        out = UsbMidi::descriptorDump();
    } else {
        server.send(404, "text/plain", "no such log");
        return;
    }
    server.send(200, "text/plain; charset=utf-8", out);
}

// The last scan's results, strongest first, one row per SSID. Never starts a
// scan: a GET is what the page polls.
void handleApiScanGet() {
    if (!apiAllowed()) return;
    int n = WiFi.scanComplete();
    Json j(s_json, sizeof(s_json));
    j.open();
    // >= 0 networks found, -1 scanning, -2 none yet or failed -- or a scan
    // past the core's 6 s limit, whose results can still arrive (the page
    // keeps asking).
    j.key("state");
    j.num(n);
    j.key("peer");
    j.boolean(RtpMidi::hasPeer());
    j.key("nets");
    j.openArr();
    if (n > 0) {
        int order[64];
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {  // insertion sort by RSSI, strongest first
            int k = order[i], m = i - 1;
            while (m >= 0 && WiFi.RSSI(order[m]) < WiFi.RSSI(k)) {
                order[m + 1] = order[m];
                m--;
            }
            order[m + 1] = k;
        }
        for (int i = 0; i < n; i++) {
            const String ssid = WiFi.SSID(order[i]);
            if (!ssid.length()) continue;  // hidden network
            bool dup = false;              // keep the strongest of each name
            for (int p = 0; p < i && !dup; p++) dup = WiFi.SSID(order[p]) == ssid;
            if (dup) continue;
            // A 32-byte SSID escapes to at most ~200 bytes, plus 64 of hex;
            // stop short of the end rather than send a broken document.
            if (j.room() < 320) break;
            j.openArr();
            j.str(ssid);
            j.num(WiFi.RSSI(order[i]));
            if (!validUtf8(ssid.c_str())) {
                // [name, rssi, hex]: the page cannot send these bytes back
                // through its UTF-8 form, so it posts the hex (ssidhex).
                String hex;
                appendHex(hex, ssid);
                j.str(hex);
            }
            j.closeArr();
        }
    }
    j.closeArr();
    j.close();
    sendJson(j);
}

// Starts an async scan. A scan is not free: the station leaves its home channel
// and hops the band for seconds, during which MIDI in BOTH directions stalls.
// Measured 2026-07-28 -- merely viewing the page mid-session was enough to
// disturb the stream, and a host that watchdogs the link can drop the session
// over it. So it is refused (409) while an RTP-MIDI peer is connected, unless
// the operator explicitly asks with force=1 ("Scan anyway") and accepts the
// glitch. With no peer -- setup time, when the list is actually wanted -- the
// page starts one on every load.
void handleApiScanPost() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    if (RtpMidi::hasPeer() && server.arg("force") != "1") {
        server.send(409, "text/plain",
                    "A MIDI session is active and a scan would interrupt it; "
                    "add force=1 to scan anyway.");
        return;
    }
    const int n = WiFi.scanComplete();
    if (n >= 0) WiFi.scanDelete();
    if (n != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true);
    server.send(202, "text/plain", "scanning\n");
}

void handleConfigPost() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    Config::Values v = Config::get();
    // Lengths are enforced here, not just by the form's maxlength (1.7.2):
    // a crafted POST could store anything, and a web password of 250+ chars
    // overflows the Arduino core's own Basic-auth buffer (its length is kept
    // in a char, which is unsigned 8-bit on this chip) on every request after.
    const char* bad = nullptr;
    String ssid = server.arg("ssid");
    // A network picked from the scan list whose name is not valid UTF-8 comes
    // as its exact bytes in hex (1.9.0): the page is UTF-8 and would otherwise
    // post a mangled name, and the device could never rejoin.
    if (server.hasArg("ssidhex") && server.arg("ssidhex").length() &&
        !ssidFromHex(server.arg("ssidhex"), ssid)) {
        server.send(400, "text/html", "Not saved: invalid WiFi SSID. Go back and correct it.");
        return;
    }
    const String pass = server.arg("pass");
    const String name = server.arg("name");
    String tip = server.arg("tip");
    tip.trim();
    const String webpass = server.arg("webpass");
    if (ssid.length() > 32) bad = "WiFi SSID (at most 32 bytes)";
    else if (pass.length() && (pass.length() < 8 || pass.length() > 64))
        bad = "WiFi password (8 to 64 characters)";
    else if (name.length() > 24) bad = "session name (at most 24 characters)";
    else if (tip.length() && !validIpv4(tip)) bad = "peer IP";
    else if (webpass.length() > 63) bad = "web UI password (at most 63 characters)";
    if (bad) {
        server.send(400, "text/html", String("Not saved: invalid ") + bad + ". Go back and correct it.");
        return;
    }
    if (ssid.length()) v.wifiSsid = ssid;
    if (pass.length()) v.wifiPass = pass;
    if (name.length()) v.sessionName = name;
    if (server.hasArg("tip")) v.targetIp = tip;
    if (server.hasArg("tport")) {
        long p = server.arg("tport").toInt();
        if (p >= 1 && p <= 65535) v.targetPort = (uint16_t)p;
    }
    // USB MIDI selection. Both come from <select>s, so anything out of range
    // is a crafted POST -- reject rather than store a value the bridge would
    // then have to second-guess at every attach.
    if (server.hasArg("uif")) {
        const String& a = server.arg("uif");
        if (!allDigits(a) || a.toInt() > 255) {
            server.send(400, "text/html", "Not saved: invalid USB MIDI interface.");
            return;
        }
        v.usbIface = (uint8_t)a.toInt();
    }
    if (server.hasArg("ucab")) {
        const String& a = server.arg("ucab");
        long n = a.toInt();
        if (!allDigits(a) || !(n <= 15 || n == Config::CABLE_ALL)) {
            server.send(400, "text/html", "Not saved: invalid USB MIDI input port.");
            return;
        }
        v.usbCable = (uint8_t)n;
    }
    if (server.hasArg("ucabo")) {
        const String& a = server.arg("ucabo");
        long n = a.toInt();
        // CABLE_ALL is meaningless outbound: sending to every cable at once
        // would just multiply traffic to the device.
        if (!allDigits(a) || !(n <= 15 || n == Config::CABLE_SAME)) {
            server.send(400, "text/html", "Not saved: invalid USB MIDI output port.");
            return;
        }
        v.usbCableOut = (uint8_t)n;
    }
    // Static IP block: validate before saving anything -- a bad value that
    // slipped into NVS would only surface as an unreachable device.
    {
        String err;
        String sip = server.hasArg("sip") ? server.arg("sip") : v.staticIp;
        String smask = server.hasArg("smask") ? server.arg("smask") : v.staticMask;
        String sgw = server.hasArg("sgw") ? server.arg("sgw") : v.staticGw;
        String sdns = server.hasArg("sdns") ? server.arg("sdns") : v.staticDns;
        sip.trim(); smask.trim(); sgw.trim(); sdns.trim();
        if (smask.length() == 0) smask = "255.255.255.0";
        if (sip.length() && (!validIpv4(sip) || sip == "0.0.0.0")) err = "static IP";
        else if (!validIpv4(smask) || !validMask(smask)) err = "subnet mask";
        else if (sgw.length() && !validIpv4(sgw)) err = "gateway";
        else if (sdns.length() && !validIpv4(sdns)) err = "DNS server";
        // lwIP only routes through an on-link gateway: one outside the subnet
        // leaves a device that joins WiFi (so no setup-AP fallback) but that
        // nothing beyond its own subnet can reach.
        else if (sip.length() && sgw.length() &&
                 (ipv4Value(sip) & ipv4Value(smask)) != (ipv4Value(sgw) & ipv4Value(smask)))
            err = "gateway (not inside the static IP's subnet)";
        else if (sip.length() && !validHostAddr(ipv4Value(sip), ipv4Value(smask)))
            err = "static IP (the subnet's network or broadcast address, or reserved)";
        else if (sip.length() && sgw.length() &&
                 (ipv4Value(sgw) == ipv4Value(sip) ||
                  !validHostAddr(ipv4Value(sgw), ipv4Value(smask))))
            err = "gateway (the static IP itself, or not a usable address)";
        if (err.length()) {
            server.send(400, "text/html",
                        "Not saved: invalid " + err + ". Go back and correct it.");
            return;
        }
        v.staticIp = sip;
        v.staticMask = smask;
        v.staticGw = sgw;
        v.staticDns = sdns;
    }
    if (server.hasArg("txp")) {
        const String& a = server.arg("txp");
        long n = a.toInt();
        // Comes from a <select>, so anything outside the fixed choice list is
        // a crafted POST -- reject rather than hand the radio a raw register
        // value.
        if (!allDigits(a) || n > 255 || !Config::txPowerValid((uint8_t)n)) {
            server.send(400, "text/html", "Not saved: invalid WiFi TX power.");
            return;
        }
        v.txPower = (uint8_t)n;
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
    if (refuseCrossOrigin()) return;
    if (!uploadAuthorized) {
        server.send(401, "text/plain", "unauthorized");
        return;
    }
    if (uploadEmpty) {
        server.send(400, "text/html", "Update failed: no firmware file was selected.");
        return;
    }
    // A truncated or corrupt image cannot get past this: Update.end() only
    // activates the slot through esp_ota_set_boot_partition(), which verifies
    // the whole image (checksum + SHA-256) first.
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
        uploadAuthorized = authOk() && sameOrigin();
        uploadEmpty = false;
        if (!uploadAuthorized) {
            Serial.println("[web] unauthorized firmware upload rejected");
            return;
        }
        Serial.printf("[web] firmware upload start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("[web] OTA begin failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        // The whole upload arrives inside ONE loop() pass: keep the loop
        // watchdog (BootGuard) fed, whoever is sending.
        BootGuard::feedWatchdog();
        if (!uploadAuthorized) return;
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (!uploadAuthorized) return;
        // "Upload & flash" with no file chosen still posts an (empty) file
        // part. Don't let Update.end() go looking for an image in a slot
        // nothing was written to.
        if (up.totalSize == 0) {
            uploadEmpty = true;
            Update.abort();
            Serial.println("[web] firmware upload was empty -- nothing flashed");
            return;
        }
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

// Settings survive; this is only a power-cycle equivalent. markStable() first,
// like every other deliberate reboot, so it never counts toward the boot guard's
// rollback threshold.
void handleRebootPost() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    Serial.println("[web] reboot requested");
    server.send(200, "text/html",
                "<meta http-equiv='refresh' content='10;url=/'>Rebooting...");
    BootGuard::markStable();
    delay(300);
    ESP.restart();
}

// Plain text for measurement scripts, a few hundred bytes, cheap enough to
// sample once a second during a load ramp without becoming part of the
// experiment. The counters are single-writer 32-bit values read live rather
// than from the 100 ms snapshot, since the VM tooling diffs them.
// New keys are only ever appended: that tooling parses these lines.
void handleDiag() {
    if (!authOk()) return server.requestAuthentication();
    String s;
    s.reserve(1024);
    s += "fw=" FW_VERSION "\nuptime_s=";
    s += String(millis() / 1000);
    s += "\nrssi=";
    s += String(WiFi.RSSI());
    s += "\npeers=";
    s += String(RtpMidi::peerCount());
    s += "\nrtp_to_usb_events=";
    s += String(MidiBridge::returnedCount());
    // Lifetime totals; the per-window equivalents ride in the out= line.
    s += "\ntx_packets_total=";
    s += String(UsbMidi::txPacketCount());
    s += "\ntx_dropped_total=";
    s += String(UsbMidi::txDropCount());
    s += "\nusb_to_rtp_events=";
    s += String(MidiBridge::forwardedCount());
    s += "\nusb_to_rtp_packets=";
    s += String(MidiBridge::uplinkPackets());
    s += "\nhealthy=";
    s += String(UsbMidi::healthy() ? 1 : 0);
    s += "\nusb_port=";
    UsbMidi::appendPortDiag(s);
    s += " stack_errors=";
    s += String(UsbMidi::stackLogCount());
    s += "\nwifi=";
    WifiNet::appendDiag(s);
    s += "\nheap=";
    s += String(ESP.getFreeHeap());
    s += "\nout=";
    UsbMidi::appendTxDiag(s);
    s += "\nin=";
    UsbMidi::appendRxDiag(s);
    {
        // 1.8.0: where the time goes. A page load should show in loop= and
        // never in midi_task=.
        MidiTask::Snapshot snap;
        MidiTask::snapshot(snap);
        // Worst case ~260 bytes with every counter at 10 digits.
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "\nmidi_task=pass_max_us=%lu pass_mean_us=%lu period_max_us=%lu "
                 "wakes_usb=%lu wakes_timer=%lu stack_free=%lu"
                 "\nloop=period_max_us=%lu\nlog_drops=%lu\nheap_min=%lu"
                 "\nstacks=loop=%lu usbh_client=%lu",
                 (unsigned long)snap.passMaxUs, (unsigned long)snap.passMeanUs,
                 (unsigned long)snap.periodMaxUs, (unsigned long)snap.wakesUsb,
                 (unsigned long)snap.wakesTimer, (unsigned long)snap.stackFree,
                 (unsigned long)BootGuard::loopPeriodMaxUs(),
                 (unsigned long)LogQueue::drops(), (unsigned long)ESP.getMinFreeHeap(),
                 (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
                 (unsigned long)UsbMidi::clientStackFree());
        s += buf;
    }
    s += "\nmac=";  // 1.9.0, the station MAC
    {
        char mac[18];
        formatMac(mac, sizeof(mac), ESP_MAC_WIFI_STA);
        s += mac;
    }
    s += "\n";
    server.send(200, "text/plain", s);
}

// POST, not GET: this mutates state, and a GET that does so is one browser
// prefetch or link-scanner away from zeroing a measurement mid-run. Auth makes
// that unlikely rather than impossible, and the correct method costs nothing.
void handleDiagReset() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    UsbMidi::resetDiag();
    MidiTask::resetStats();
    BootGuard::resetLoopStats();
    server.send(200, "text/plain", "ok\n");
}

void handleResetPost() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    Serial.println("[web] factory reset requested");
    server.send(200, "text/html",
                "<meta http-equiv='refresh' content='10;url=/'>Settings erased. Rebooting...");
    Config::wipeAll();
    delay(300);
    ESP.restart();
}
}  // namespace

void WebUi::begin() {
    // Authorization is always collected. Origin is what sameOrigin() checks;
    // the other two serve the page (gzip, and the ETag revalidation).
    static const char* HEADERS[] = {"Origin", "Accept-Encoding", "If-None-Match"};
    server.collectHeaders(HEADERS, 3);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/config", HTTP_GET, handleApiConfig);
    server.on("/api/log", HTTP_GET, handleApiLog);
    server.on("/api/scan", HTTP_GET, handleApiScanGet);
    server.on("/api/scan", HTTP_POST, handleApiScanPost);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
    server.on("/reset", HTTP_POST, handleResetPost);
    server.on("/reboot", HTTP_POST, handleRebootPost);
    server.on("/diag", HTTP_GET, handleDiag);
    server.on("/diagreset", HTTP_POST, handleDiagReset);
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    Serial.println("[web] config UI on http://esp32-midi.local/");
}

void WebUi::tick() {
    server.handleClient();
}
