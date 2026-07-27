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
