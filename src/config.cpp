#include "config.h"

#include <Preferences.h>

namespace {
Config::Values values;
Preferences prefs;
}  // namespace

void Config::load() {
    // No baked-in credentials: an unconfigured device (empty SSID) opens the
    // setup AP portal instead of joining a network.
    prefs.begin("midicfg", false);  // read-write so first boot creates the namespace
    values.wifiSsid = prefs.getString("ssid", "");
    values.wifiPass = prefs.getString("pass", "");
    values.sessionName = prefs.getString("name", "ESP32-MIDI");
    values.targetIp = prefs.getString("tip", "");
    values.targetPort = prefs.getUShort("tport", 5004);
    // Default login is admin/midimidi (documented in the README, not a secret).
    // A stored empty string means the user explicitly removed protection.
    values.webPass = prefs.getString("wpass", "midimidi");
    values.staticIp = prefs.getString("sip", "");
    values.staticMask = prefs.getString("smask", "255.255.255.0");
    values.staticGw = prefs.getString("sgw", "");
    values.staticDns = prefs.getString("sdns", "");
    // Defaults reproduce the pre-1.3.0 hard-coded behavior: first MIDI
    // interface the device offers, virtual cable 0 only.
    values.usbIface = prefs.getUChar("uif", IFACE_AUTO);
    values.usbCable = prefs.getUChar("ucab", 0);
    if (values.usbCable > 15 && values.usbCable != CABLE_ALL) values.usbCable = 0;
    prefs.end();
}

bool Config::save(const Values& v) {
    if (!prefs.begin("midicfg", false)) return false;
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
    prefs.end();
    values = v;
    return true;
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
