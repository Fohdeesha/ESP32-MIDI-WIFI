#include "config.h"

#include <Preferences.h>

namespace {
Config::Values values;
Preferences prefs;

// Every setting as stored, with the default for anything never stored -- and
// no validation, which load() adds.
void readStored(Config::Values& v) {
    // No baked-in credentials: an unconfigured device (empty SSID) opens the
    // setup AP portal instead of joining a network.
    v.wifiSsid = prefs.getString("ssid", "");
    v.wifiPass = prefs.getString("pass", "");
    v.sessionName = prefs.getString("name", "ESP32-MIDI");
    v.targetIp = prefs.getString("tip", "");
    v.targetPort = prefs.getUShort("tport", 5004);
    // Default login is admin/midimidi (documented in the README, not a secret).
    // A stored empty string means the user explicitly removed protection.
    v.webPass = prefs.getString("wpass", "midimidi");
    v.staticIp = prefs.getString("sip", "");
    v.staticMask = prefs.getString("smask", "255.255.255.0");
    v.staticGw = prefs.getString("sgw", "");
    v.staticDns = prefs.getString("sdns", "");
    // Defaults reproduce the pre-1.3.0 hard-coded behavior: first MIDI
    // interface the device offers, virtual cable 0 only.
    v.usbIface = prefs.getUChar("uif", Config::IFACE_AUTO);
    v.usbCable = prefs.getUChar("ucab", 0);
    v.usbCableOut = prefs.getUChar("ucabo", Config::CABLE_SAME);
    v.txPower = prefs.getUChar("txp", Config::TX_POWER_DEFAULT);
}

void writeAll(const Config::Values& v) {
    prefs.putString("ssid", v.wifiSsid);
    prefs.putString("pass", v.wifiPass);
    prefs.putString("name", v.sessionName);
    prefs.putString("tip", v.targetIp);
    prefs.putUShort("tport", v.targetPort);
    prefs.putString("wpass", v.webPass);
    prefs.putString("sip", v.staticIp);
    prefs.putString("smask", v.staticMask);
    prefs.putString("sgw", v.staticGw);
    prefs.putString("sdns", v.staticDns);
    prefs.putUChar("uif", v.usbIface);
    prefs.putUChar("ucab", v.usbCable);
    prefs.putUChar("ucabo", v.usbCableOut);
    prefs.putUChar("txp", v.txPower);
}

bool sameSettings(const Config::Values& a, const Config::Values& b) {
    return a.wifiSsid == b.wifiSsid && a.wifiPass == b.wifiPass &&
           a.sessionName == b.sessionName && a.targetIp == b.targetIp &&
           a.targetPort == b.targetPort && a.webPass == b.webPass &&
           a.staticIp == b.staticIp && a.staticMask == b.staticMask &&
           a.staticGw == b.staticGw && a.staticDns == b.staticDns &&
           a.usbIface == b.usbIface && a.usbCable == b.usbCable &&
           a.usbCableOut == b.usbCableOut && a.txPower == b.txPower;
}
}  // namespace

void Config::load() {
    prefs.begin("midicfg", false);  // read-write so first boot creates the namespace
    readStored(values);
    prefs.end();
    if (values.usbCable > 15 && values.usbCable != CABLE_ALL) values.usbCable = 0;
    if (values.usbCableOut > 15 && values.usbCableOut != CABLE_SAME) {
        values.usbCableOut = CABLE_SAME;
    }
    if (!txPowerValid(values.txPower)) values.txPower = TX_POWER_DEFAULT;
}

bool Config::save(const Values& v) {
    if (!prefs.begin("midicfg", false)) return false;
    writeAll(v);
    // Read it all back (1.7.2). A put that fails -- NVS full, a flash error --
    // only returns 0, which putString() also returns for a successful empty
    // string, so this used to report "Saved" regardless and the settings were
    // lost at the reboot that follows. What reads back is what the next boot
    // will load.
    Values stored;
    readStored(stored);
    bool ok = sameSettings(stored, v);
    if (!ok) {
        // Some keys may already hold the new values: put the running settings
        // back, so the next boot does not load a mix (a new SSID with the old
        // password). Best effort -- the same fault may refuse this too.
        writeAll(values);
        Serial.println("[cfg] save failed: settings did not read back from NVS");
    }
    prefs.end();
    if (ok) values = v;
    return ok;
}

void Config::wipeAll() {
    prefs.begin("midicfg", false);
    prefs.clear();
    prefs.end();
    prefs.begin("bootguard", false);
    prefs.clear();
    prefs.end();
}

const Config::Values& Config::get() {
    return values;
}
