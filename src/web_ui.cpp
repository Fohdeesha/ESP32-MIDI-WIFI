#include "web_ui.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "boot_guard.h"
#include "buffered_udp.h"
#include "config.h"
#include "log_queue.h"
#include "midi_bridge.h"
#include "midi_task.h"
#include "rtp_midi.h"
#include "usb_midi_host.h"
// Generated from web/index.html by tools/embed_web.py, under the build dir.
#include "web_page.h"
#include "wifi_net.h"

// tools/patch_webserver.py bounds two waits in the core's WebServer that let
// one client with no password hang the server, and with it the board (1.10.0,
// see the script). Never build against a copy it has not patched.
#if !defined(ESP32_MIDI_WIFI_WEBSERVER_PATCHSET) || ESP32_MIDI_WIFI_WEBSERVER_PATCHSET != 1
#error "WebServer is the core's unpatched copy: the build must use tools/patch_webserver.py's"
#endif

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
// The upload handler has already answered (it refused the file mid-upload).
// If the refusal came with the last chunk the request still completes, and
// handleUpdatePost() must not answer a second time.
bool uploadAnswered = false;

// "" = protection off. Basic auth, fixed username "admin". LAN-grade only.
bool authOk() {
    const String& p = Config::get().webPass;
    if (p.length() == 0) return true;
    return server.authenticate("admin", p.c_str());
}

// Valid dotted-quad IPv4, e.g. "192.0.2.10". IPAddress::fromString alone
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

bool refuseCrossOrigin(const char* what = "to change settings") {
    if (!sameOrigin()) {
        server.send(403, "text/plain", "Refused: request came from another site.");
        return true;
    }
    if (!hostOk()) {
        server.send(403, "text/plain",
                    String("Refused: ") + what + ", open this page as http://" +
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

// The exact bytes of a name sent as hex: an SSID from the scan list (see
// handleConfigPost), or a text setting in an imported file that is not UTF-8.
// False on anything but 1 to maxBytes bytes of hex with no NUL.
bool bytesFromHex(const String& hex, size_t maxBytes, String& out) {
    if (hex.length() < 2 || hex.length() > maxBytes * 2 || hex.length() % 2) return false;
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

// ── Setting checks (1.10.0) ─────────────────────────────────────────────────
// Every check a setting gets on its way into NVS, shared by the config form
// and an imported settings file, so neither can store what the other refuses.
// The text ones name what is wrong, or return nullptr.
//
// Lengths are enforced here, not just by the form's maxlength (1.7.2): a
// crafted POST could store anything, and a web password of 250+ chars
// overflows the Arduino core's own Basic-auth buffer (its length is kept in a
// char, which is unsigned 8-bit on this chip) on every request after.
const char* badSsid(const String& s) {
    return s.length() > 32 ? "WiFi SSID (at most 32 bytes)" : nullptr;
}

const char* badWifiPass(const String& s) {
    return s.length() && (s.length() < 8 || s.length() > 64) ? "WiFi password (8 to 64 characters)"
                                                              : nullptr;
}

// Bytes, as the library stores the name: a 24-character name with accented
// letters is longer than that.
const char* badSessionName(const String& s) {
    return s.length() < 1 || s.length() > 24 ? "session name (1 to 24 bytes)" : nullptr;
}

const char* badPeerIp(const String& s) {
    return s.length() && !validIpv4(s) ? "peer IP" : nullptr;
}

const char* badWebPass(const String& s) {
    return s.length() > 63 ? "web UI password (at most 63 characters)" : nullptr;
}

// Plain decimal digits and nothing else, at most `max`. toInt() alone would
// take "5004abc" as 5004 and wrap a long run of digits.
bool decimalValue(const String& s, uint32_t max, uint32_t& out) {
    if (!allDigits(s) || s.length() > 9) return false;
    const uint32_t v = strtoul(s.c_str(), nullptr, 10);
    if (v > max) return false;
    out = v;
    return true;
}

bool peerPortValue(const String& s, uint16_t& out) {
    uint32_t v;
    if (!decimalValue(s, 65535, v) || v == 0) return false;
    out = (uint16_t)v;
    return true;
}

// The USB MIDI selection and the TX power come from <select>s, so anything
// outside their lists is a crafted request -- refused rather than stored for
// the bridge to second-guess at every attach, or handed to the radio as a raw
// register value.
bool ifaceValue(const String& s, uint8_t& out) {
    uint32_t v;
    if (!decimalValue(s, 255, v)) return false;
    out = (uint8_t)v;
    return true;
}

bool cableInValue(const String& s, uint8_t& out) {
    uint32_t v;
    if (!decimalValue(s, 255, v) || !(v <= 15 || v == Config::CABLE_ALL)) return false;
    out = (uint8_t)v;
    return true;
}

// CABLE_ALL is meaningless outbound: sending to every cable at once would
// just multiply traffic to the device.
bool cableOutValue(const String& s, uint8_t& out) {
    uint32_t v;
    if (!decimalValue(s, 255, v) || !(v <= 15 || v == Config::CABLE_SAME)) return false;
    out = (uint8_t)v;
    return true;
}

bool txPowerValue(const String& s, uint8_t& out) {
    uint32_t v;
    if (!decimalValue(s, 255, v) || !Config::txPowerValid((uint8_t)v)) return false;
    out = (uint8_t)v;
    return true;
}

// The static IP block, checked as a whole since each field constrains the
// others. Trims all four and fills in the default mask first. A bad value that
// slipped into NVS would only surface as an unreachable device.
const char* badStaticIp(String& sip, String& smask, String& sgw, String& sdns) {
    sip.trim();
    smask.trim();
    sgw.trim();
    sdns.trim();
    if (smask.length() == 0) smask = "255.255.255.0";
    if (sip.length() && (!validIpv4(sip) || sip == "0.0.0.0")) return "static IP";
    if (!validIpv4(smask) || !validMask(smask)) return "subnet mask";
    if (sgw.length() && !validIpv4(sgw)) return "gateway";
    if (sdns.length() && !validIpv4(sdns)) return "DNS server";
    if (!sip.length()) return nullptr;
    const uint32_t ip = ipv4Value(sip), mask = ipv4Value(smask);
    // lwIP only routes through an on-link gateway: one outside the subnet
    // leaves a device that joins WiFi (so no setup-AP fallback) but that
    // nothing beyond its own subnet can reach.
    if (sgw.length() && (ip & mask) != (ipv4Value(sgw) & mask))
        return "gateway (not inside the static IP's subnet)";
    if (!validHostAddr(ip, mask))
        return "static IP (the subnet's network or broadcast address, or reserved)";
    if (sgw.length() && (ipv4Value(sgw) == ip || !validHostAddr(ipv4Value(sgw), mask)))
        return "gateway (the static IP itself, or not a usable address)";
    return nullptr;
}

// One JSON document built into a fixed buffer, with commas placed
// automatically. A document that does not fit is reported (ok() == false),
// never sent cut short. Strings are escaped for JSON and additionally for
// HTML (<, >, &), so even a mis-sniffed response could not carry markup.
// With lines = true (the settings export, a flat object meant to be read and
// edited by hand) each member goes on its own line and only JSON's own
// escapes are used: that file is a download, never shown as a page.
class Json {
public:
    Json(char* buf, size_t cap, bool lines = false) : b_(buf), cap_(cap), lines_(lines) {
        b_[0] = '\0';
    }
    void open() { sep(); put('{'); comma_ = false; }
    void close() {
        if (lines_) put('\n');
        put('}');
        if (lines_) put('\n');
        comma_ = true;
    }
    void openArr() { sep(); put('['); comma_ = false; }
    void closeArr() { put(']'); comma_ = true; }
    void key(const char* k) {
        sep();
        if (lines_) raw("\n  ");
        quoted(k);
        put(':');
        if (lines_) put(' ');
        comma_ = false;
    }
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
            } else if (c < 0x20 || c == 0x7F ||
                       (!lines_ && (c == '<' || c == '>' || c == '&'))) {
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
    bool lines_;
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

// Saves the settings and reboots into them; reports the failure otherwise.
void saveAndReboot(const Config::Values& v, const char* done) {
    const bool ok = Config::save(v);
    server.send(ok ? 200 : 500, "text/html",
                ok ? String("<meta http-equiv='refresh' content='8;url=/'>") + done
                   : String("Failed to save config"));
    if (ok) {
        BootGuard::markStable();
        delay(300);
        ESP.restart();
    }
}

void handleConfigPost() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    Config::Values v = Config::get();
    String ssid = server.arg("ssid");
    // A network picked from the scan list whose name is not valid UTF-8 comes
    // as its exact bytes in hex (1.9.0): the page is UTF-8 and would otherwise
    // post a mangled name, and the device could never rejoin.
    if (server.hasArg("ssidhex") && server.arg("ssidhex").length() &&
        !bytesFromHex(server.arg("ssidhex"), 32, ssid)) {
        server.send(400, "text/html", "Not saved: invalid WiFi SSID. Go back and correct it.");
        return;
    }
    // A blank SSID, WiFi password or session name keeps the stored one.
    const String pass = server.arg("pass");
    const String name = server.arg("name");
    String tip = server.arg("tip");
    tip.trim();
    const String webpass = server.arg("webpass");
    const String tport = server.arg("tport");
    const char* bad = badSsid(ssid);
    if (!bad) bad = badWifiPass(pass);
    if (!bad && name.length()) bad = badSessionName(name);
    if (!bad) bad = badPeerIp(tip);
    if (!bad) bad = badWebPass(webpass);
    // A blank port keeps the stored one; anything else must be a port (1.10.0:
    // until then a bad one was silently ignored).
    if (!bad && tport.length() && !peerPortValue(tport, v.targetPort))
        bad = "peer port (1 to 65535)";
    if (bad) {
        server.send(400, "text/html", String("Not saved: invalid ") + bad + ". Go back and correct it.");
        return;
    }
    if (ssid.length()) v.wifiSsid = ssid;
    if (pass.length()) v.wifiPass = pass;
    if (name.length()) v.sessionName = name;
    if (server.hasArg("tip")) v.targetIp = tip;
    if (server.hasArg("uif") && !ifaceValue(server.arg("uif"), v.usbIface)) {
        server.send(400, "text/html", "Not saved: invalid USB MIDI interface.");
        return;
    }
    if (server.hasArg("ucab") && !cableInValue(server.arg("ucab"), v.usbCable)) {
        server.send(400, "text/html", "Not saved: invalid USB MIDI input port.");
        return;
    }
    if (server.hasArg("ucabo") && !cableOutValue(server.arg("ucabo"), v.usbCableOut)) {
        server.send(400, "text/html", "Not saved: invalid USB MIDI output port.");
        return;
    }
    {
        String sip = server.hasArg("sip") ? server.arg("sip") : v.staticIp;
        String smask = server.hasArg("smask") ? server.arg("smask") : v.staticMask;
        String sgw = server.hasArg("sgw") ? server.arg("sgw") : v.staticGw;
        String sdns = server.hasArg("sdns") ? server.arg("sdns") : v.staticDns;
        if (const char* err = badStaticIp(sip, smask, sgw, sdns)) {
            server.send(400, "text/html",
                        String("Not saved: invalid ") + err + ". Go back and correct it.");
            return;
        }
        v.staticIp = sip;
        v.staticMask = smask;
        v.staticGw = sgw;
        v.staticDns = sdns;
    }
    if (server.hasArg("txp") && !txPowerValue(server.arg("txp"), v.txPower)) {
        server.send(400, "text/html", "Not saved: invalid WiFi TX power.");
        return;
    }
    if (server.hasArg("clearpass") && server.arg("clearpass") == "1") {
        v.webPass = "";
    } else if (webpass.length()) {
        v.webPass = webpass;
    }
    saveAndReboot(v, "Saved. Rebooting...");
}

// ── Settings export and import (1.10.0) ─────────────────────────────────────
// One JSON file holding every setting, the WiFi and web UI passwords
// included, so a board can be backed up, restored after a factory reset, or
// cloned. The keys are the config form's field names, the same ones
// /api/config uses. A text setting that is not valid UTF-8 (an SSID stored by
// an older version, say) goes out as "<key>_hex", its exact bytes, since a
// JSON file is UTF-8 text.
constexpr const char* SETTINGS_FORMAT = "esp32-midi-wifi-settings";
constexpr uint32_t SETTINGS_VERSION = 1;
// A real export is well under 1 kB, even with every name at its longest.
constexpr size_t IMPORT_MAX_BYTES = 4096;

void textSetting(Json& j, const char* key, const String& v) {
    if (validUtf8(v.c_str())) {
        j.key(key);
        j.str(v);
        return;
    }
    char hexKey[16];
    snprintf(hexKey, sizeof(hexKey), "%s_hex", key);
    String hex;
    appendHex(hex, v);
    j.key(hexKey);
    j.str(hex);
}

// A download, so the same checks as anything that changes settings: it holds
// the passwords, and with DNS rebinding a foreign page could otherwise read it
// using the documented default login.
void handleExport() {
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin("to export the settings")) return;
    const Config::Values& c = Config::get();
    Json j(s_json, sizeof(s_json), true);
    j.open();
    j.key("format");
    j.str(SETTINGS_FORMAT);
    j.key("version");
    j.unum(SETTINGS_VERSION);
    j.key("firmware");
    j.str(FW_VERSION);
    textSetting(j, "ssid", c.wifiSsid);
    textSetting(j, "pass", c.wifiPass);
    textSetting(j, "name", c.sessionName);
    textSetting(j, "tip", c.targetIp);
    j.key("tport");
    j.unum(c.targetPort);
    textSetting(j, "sip", c.staticIp);
    textSetting(j, "smask", c.staticMask);
    textSetting(j, "sgw", c.staticGw);
    textSetting(j, "sdns", c.staticDns);
    j.key("uif");
    j.unum(c.usbIface);
    j.key("ucab");
    j.unum(c.usbCable);
    j.key("ucabo");
    j.unum(c.usbCableOut);
    j.key("txp");
    j.unum(c.txPower);
    textSetting(j, "webpass", c.webPass);
    j.close();
    if (!j.ok()) {
        server.send(500, "text/plain", "response too large");
        return;
    }
    // Named after the board (the end of its MAC), so files from several
    // boards don't overwrite each other in a downloads folder.
    uint8_t m[6] = {};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"esp32-midi-settings-%02x%02x%02x.json\"",
             m[3], m[4], m[5]);
    server.sendHeader("Content-Disposition", disp);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    server.send_P(200, "application/json; charset=utf-8", j.c_str(), j.length());
}

// Reads a flat JSON object -- string keys, each value a string or a whole
// number -- which is all the export writes. Strings are decoded, escapes and
// surrogate pairs included, to UTF-8; numbers keep their digits. Anything else
// (nesting, fractions, true/false/null, a key given twice, text after the end)
// is refused, with what was wrong and where.
struct JsonMember {
    String key;
    String value;
    bool isText;
};

class FlatJsonReader {
public:
    FlatJsonReader(const char* p, size_t n) : start_(p), p_(p), end_(p + n) {}

    const char* read(JsonMember* out, int cap, int& count) {
        count = 0;
        ws();
        if (!at('{')) return "it is not a JSON object";
        p_++;
        ws();
        if (at('}')) {
            p_++;
        } else {
            for (;;) {
                ws();
                if (count == cap) return "it has too many entries";
                JsonMember& m = out[count];
                if (!at('"') || !string(m.key)) return "a name is not a valid JSON string";
                ws();
                if (!at(':')) return "a ':' is missing";
                p_++;
                ws();
                if (at('"')) {
                    if (!string(m.value)) return "a value is not a valid JSON string";
                    m.isText = true;
                } else if (p_ < end_ && (*p_ == '-' || (*p_ >= '0' && *p_ <= '9'))) {
                    if (!number(m.value)) return "a number is not a whole number";
                    m.isText = false;
                } else {
                    return "a value is not text or a whole number";
                }
                for (int i = 0; i < count; i++) {
                    if (out[i].key == m.key) return "a setting is given twice";
                }
                count++;
                ws();
                if (at(',')) {
                    p_++;
                    continue;
                }
                if (at('}')) {
                    p_++;
                    break;
                }
                return "a ',' or '}' is missing";
            }
        }
        ws();
        return p_ == end_ ? nullptr : "there is text after the end of the JSON object";
    }
    size_t offset() const { return p_ - start_; }

private:
    bool at(char c) const { return p_ < end_ && *p_ == c; }
    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
    }
    int hex4() {
        if (end_ - p_ < 4) return -1;
        int v = 0;
        for (int i = 0; i < 4; i++) {
            const char ch = *p_++;
            const int d = ch >= '0' && ch <= '9'   ? ch - '0'
                          : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                          : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                   : -1;
            if (d < 0) return -1;
            v = v << 4 | d;
        }
        return v;
    }
    static void putUtf8(String& s, uint32_t cp) {
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | cp >> 6);
            s += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += (char)(0xE0 | cp >> 12);
            s += (char)(0x80 | (cp >> 6 & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | cp >> 18);
            s += (char)(0x80 | (cp >> 12 & 0x3F));
            s += (char)(0x80 | (cp >> 6 & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool string(String& out) {
        p_++;  // the opening quote
        String s;
        while (p_ < end_) {
            const uint8_t c = (uint8_t)*p_++;
            if (c == '"') {
                out = s;
                return true;
            }
            if (c < 0x20) return false;  // JSON wants control characters escaped
            if (c != '\\') {
                s += (char)c;
                continue;
            }
            if (p_ == end_) return false;
            switch (*p_++) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    int cp = hex4();
                    if (cp < 0) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // a pair: 🎹
                        if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return false;
                        p_ += 2;
                        const int lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // the second half of a pair, alone
                    }
                    if (cp == 0) return false;  // settings are C strings
                    putUtf8(s, (uint32_t)cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;  // no closing quote
    }
    bool number(String& out) {
        const char* s = p_;
        if (*p_ == '-') p_++;
        if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
        if (*p_ == '0' && p_ + 1 < end_ && p_[1] >= '0' && p_[1] <= '9') return false;
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) return false;
        out = "";
        for (; s < p_; s++) out += *s;
        return true;
    }
    const char* start_;
    const char* p_;
    const char* end_;
};

// The text settings an imported file may set, as "<key>" or "<key>_hex".
struct TextKey {
    const char* key;
    String Config::Values::*field;
};
const TextKey TEXT_KEYS[] = {
    {"ssid", &Config::Values::wifiSsid},    {"pass", &Config::Values::wifiPass},
    {"name", &Config::Values::sessionName}, {"tip", &Config::Values::targetIp},
    {"sip", &Config::Values::staticIp},     {"smask", &Config::Values::staticMask},
    {"sgw", &Config::Values::staticGw},     {"sdns", &Config::Values::staticDns},
    {"webpass", &Config::Values::webPass},
};

// ...and the one-byte numbers, each with the check its <select> implies.
struct ByteKey {
    const char* key;
    uint8_t Config::Values::*field;
    bool (*parse)(const String&, uint8_t&);
};
const ByteKey BYTE_KEYS[] = {
    {"uif", &Config::Values::usbIface, ifaceValue},
    {"ucab", &Config::Values::usbCable, cableInValue},
    {"ucabo", &Config::Values::usbCableOut, cableOutValue},
    {"txp", &Config::Values::txPower, txPowerValue},
};

// Applies a settings file to v. A setting the file leaves out keeps its
// current value; one it gives is set exactly, blank included (a blank WiFi
// password is an open network, a blank web password turns the login off),
// after the same checks the config form applies. Returns what is wrong, or
// nullptr; `applied` counts the settings the file gave and `ignored` lists
// any key this firmware does not know (a newer version's, or a typo).
String applySettingsFile(char* doc, size_t len, Config::Values& v, int& applied,
                         String& ignored) {
    applied = 0;
    // Windows editors like to start a UTF-8 file with a byte-order mark.
    if (len >= 3 && memcmp(doc, "\xEF\xBB\xBF", 3) == 0) {
        doc += 3;
        len -= 3;
    }
    doc[len] = '\0';
    if (memchr(doc, 0, len) || !validUtf8(doc)) return "the file is not UTF-8 text";
    static JsonMember members[32];
    int n = 0;
    FlatJsonReader reader(doc, len);
    if (const char* err = reader.read(members, 32, n)) {
        return String("the file is not a settings file: ") + err + " (byte " +
               String((unsigned long)reader.offset()) + ")";
    }
    const JsonMember* format = nullptr;
    for (int i = 0; i < n; i++) {
        if (members[i].key == "format") format = &members[i];
    }
    if (!format || !format->isText || format->value != SETTINGS_FORMAT) {
        return "the file is not a settings file exported by this firmware";
    }
    for (int i = 0; i < n; i++) {
        const JsonMember& m = members[i];
        if (m.key == "format" || m.key == "firmware") continue;  // firmware: for people
        if (m.key == "version") {
            if (m.isText || m.value != String(SETTINGS_VERSION))
                return "the file's format version is not one this firmware reads";
            continue;
        }
        // A blank SSID would leave the board on its setup hotspot, reachable
        // only on site; the config form cannot store one either.
        if (m.key == "ssid" && m.isText && !m.value.length())
            return "the file has a blank WiFi SSID (from a board never set up?)";
        bool known = false;
        for (const TextKey& t : TEXT_KEYS) {
            const String hexKey = String(t.key) + "_hex";
            if (m.key != t.key && m.key != hexKey) continue;
            known = true;
            for (int k = 0; k < n; k++) {
                if (k != i && (members[k].key == t.key || members[k].key == hexKey))
                    return String("the file gives both ") + t.key + " and " + hexKey;
            }
            if (!m.isText) return String("invalid ") + m.key + " (not text)";
            if (m.key == hexKey) {
                if (!bytesFromHex(m.value, 64, v.*t.field))
                    return String("invalid ") + m.key + " (not hex bytes)";
            } else {
                v.*t.field = m.value;
            }
            break;
        }
        for (const ByteKey& b : BYTE_KEYS) {
            if (m.key != b.key) continue;
            known = true;
            if (m.isText) return String("invalid ") + m.key + " (not a number)";
            if (!b.parse(m.value, v.*b.field)) return String("invalid ") + m.key;
            break;
        }
        if (m.key == "tport") {
            known = true;
            if (m.isText) return String("invalid ") + m.key + " (not a number)";
            if (!peerPortValue(m.value, v.targetPort)) return "invalid tport (1 to 65535)";
        }
        if (known) {
            applied++;
        } else if (ignored.length() < 120) {
            ignored += ignored.length() ? ", " : "";
            ignored += m.key.substring(0, 24);
        }
    }
    v.targetIp.trim();
    const char* bad = badSsid(v.wifiSsid);
    if (!bad) bad = badWifiPass(v.wifiPass);
    if (!bad) bad = badSessionName(v.sessionName);
    if (!bad) bad = badPeerIp(v.targetIp);
    if (!bad) bad = badWebPass(v.webPass);
    if (!bad) bad = badStaticIp(v.staticIp, v.staticMask, v.staticGw, v.staticDns);
    if (bad) return String("invalid ") + bad;
    if (applied == 0) return "the file holds no settings";
    return String();
}

// The file being imported, as it arrives. It goes into s_json: an upload and
// the handler that reads it run inside one handleClient() call, so no JSON
// request can come between them.
size_t s_importLen = 0;
bool s_importGot = false;     // this request carried a file part
bool s_importNamed = false;   // ...with a file in it (a form posts an empty part without)
bool s_importAllowed = false;
bool s_importTooBig = false;

void handleImportUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        s_importGot = true;
        s_importNamed = up.filename.length() > 0;
        s_importAllowed = authOk() && sameOrigin() && hostOk();
        s_importLen = 0;
        s_importTooBig = false;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        // A refused sender's file is read to the end within the server's 20 s
        // request limit and answered by handleImportPost().
        if (!s_importAllowed || s_importTooBig) return;
        if (s_importLen + up.currentSize > IMPORT_MAX_BYTES) {
            // The wrong file picked, most likely: answer now and stop reading
            // instead of taking in the rest of it.
            s_importTooBig = true;
            server.send(400, "text/html",
                        "Not imported: the file is too large to be a settings file. "
                        "Nothing was changed.");
            server.abortUpload();
            return;
        }
        memcpy(s_json + s_importLen, up.buf, up.currentSize);
        s_importLen += up.currentSize;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        s_importGot = false;  // the request ends here: no handler follows
        s_importTooBig = false;
    }
}

String htmlEscape(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        const char c = s[i];
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else if (c == '\'') o += "&#39;";
        else o += c;
    }
    return o;
}

void handleImportPost() {
    const bool got = s_importGot;
    s_importGot = false;  // a later request must never see this upload
    if (s_importTooBig) {
        // Refused and answered with its last chunk (handleImportUpload); the
        // request completed anyway, so it lands here: answer only once.
        s_importTooBig = false;
        return;
    }
    if (!authOk()) return server.requestAuthentication();
    if (refuseCrossOrigin()) return;
    if (!got || !s_importAllowed || (s_importLen == 0 && !s_importNamed)) {
        server.send(400, "text/html", "Not imported: no settings file was selected.");
        return;
    }
    if (s_importLen == 0) {
        server.send(400, "text/html", "Not imported: the file is empty.");
        return;
    }
    Config::Values v = Config::get();
    int applied = 0;
    String ignored;
    const String err = applySettingsFile(s_json, s_importLen, v, applied, ignored);
    if (err.length()) {
        server.send(400, "text/html",
                    "Not imported: " + htmlEscape(err) + ". Nothing was changed.");
        return;
    }
    String done = "Imported " + String(applied) + (applied == 1 ? " setting" : " settings") +
                  ". Rebooting...";
    if (ignored.length()) {
        done += "<br>Not settings this firmware has, so ignored: " + htmlEscape(ignored);
    }
    saveAndReboot(v, done.c_str());
}

void handleUpdatePost() {
    if (uploadAnswered) {
        uploadAnswered = false;
        return;
    }
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
        // The Host check belongs here too (1.10.0): Update.end() below already
        // switches the boot slot, so refusing a DNS-rebinding page only in
        // handleUpdatePost() came after its firmware was in place for the next
        // reboot.
        uploadAuthorized = authOk() && sameOrigin() && hostOk();
        uploadEmpty = false;
        uploadAnswered = false;
        if (!uploadAuthorized) {
            // Read to the end and answered by handleUpdatePost(), within the
            // patched server's 20 s request limit (tools/patch_webserver.py).
            Serial.println("[web] unauthorized firmware upload rejected");
            return;
        }
        // An image can take longer than the request limit on a slow link.
        server.allowLongUpload();
        Serial.printf("[web] firmware upload start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("[web] OTA begin failed: %s\n", Update.errorString());
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!uploadAuthorized) return;
        // The whole upload arrives inside ONE loop() pass: keep the loop
        // watchdog (BootGuard) fed. Only for an authorized upload (1.10.0):
        // fed for anyone, a client trickling a refused one held loop() for good.
        BootGuard::feedWatchdog();
        if (Update.write(up.buf, up.currentSize) != up.currentSize || Update.hasError()) {
            // Not an image, or too big for the slot: answer now and stop
            // reading, rather than take in the rest of a file that cannot be
            // flashed. The ABORTED callback that follows cleans up.
            server.send(500, "text/html", String("Update failed: ") + Update.errorString());
            server.abortUpload();
            uploadAuthorized = false;
            uploadAnswered = true;
        }
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
        uploadAnswered = false;  // the request ends here: no handler follows
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

// Plain text for measurement scripts, about 1.2 kB, cheap enough to sample
// once a second during a load ramp without becoming part of the experiment.
// The traffic counters are single-writer 32-bit values read live rather than
// from the 100 ms snapshot, since the scripts diff them; the midi_task= line
// is the snapshot's. New keys are only ever appended: the scripts parse
// these lines.
void handleDiag() {
    if (!authOk()) return server.requestAuthentication();
    String s;
    s.reserve(1536);
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
        // Worst case ~410 bytes with every counter at 10 digits. The 1.9.1
        // keys end their lines: existing keys keep their place.
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "\nmidi_task=pass_max_us=%lu pass_mean_us=%lu period_max_us=%lu "
                 "wakes_usb=%lu wakes_timer=%lu stack_free=%lu rtp_max_us=%lu "
                 "bridge_max_us=%lu health_max_us=%lu rtp_msgs_max=%lu pass_max_at_s=%lu "
                 "wakes_net=%lu"
                 "\nloop=period_max_us=%lu\nlog_drops=%lu\nheap_min=%lu"
                 "\nstacks=loop=%lu usbh_client=%lu udp_rx=%lu",
                 (unsigned long)snap.passMaxUs, (unsigned long)snap.passMeanUs,
                 (unsigned long)snap.periodMaxUs, (unsigned long)snap.wakesUsb,
                 (unsigned long)snap.wakesTimer, (unsigned long)snap.stackFree,
                 (unsigned long)snap.rtpMaxUs, (unsigned long)snap.bridgeMaxUs,
                 (unsigned long)snap.healthMaxUs, (unsigned long)snap.rtpMsgsMax,
                 (unsigned long)(snap.passMaxAtMs / 1000), (unsigned long)snap.wakesNet,
                 (unsigned long)BootGuard::loopPeriodMaxUs(),
                 (unsigned long)LogQueue::drops(), (unsigned long)ESP.getMinFreeHeap(),
                 (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
                 (unsigned long)UsbMidi::clientStackFree(),
                 (unsigned long)BufferedUDP::taskStackFree());
        s += buf;
    }
    s += "\nmac=";  // 1.9.0, the station MAC
    {
        char mac[18];
        formatMac(mac, sizeof(mac), ESP_MAC_WIFI_STA);
        s += mac;
    }
    {
        // 1.9.1: the RTP-MIDI receive queues (buffered_udp.h). full= counts
        // times one was full with datagrams still waiting in lwIP, which
        // drops past 6 of them; it should stay 0. task=0 means the receive
        // task never started and the MIDI task polls instead.
        char buf[112];
        snprintf(buf, sizeof(buf), "\nrtp_rx=queued_max=%lu full=%lu slots=%lu psram=%d task=%d",
                 (unsigned long)BufferedUDP::queuedMax(), (unsigned long)BufferedUDP::fullCount(),
                 (unsigned long)BufferedUDP::slotsEach(), BufferedUDP::inPsram() ? 1 : 0,
                 BufferedUDP::taskRunning() ? 1 : 0);
        s += buf;
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
    server.on("/config/export", HTTP_GET, handleExport);
    server.on("/config/import", HTTP_POST, handleImportPost, handleImportUpload);
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
