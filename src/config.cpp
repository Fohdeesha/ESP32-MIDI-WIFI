#include "config.h"

#include <Preferences.h>

#include "secrets.h"

namespace {
Config::Values values;
Preferences prefs;
}  // namespace

void Config::load() {
    prefs.begin("midicfg", false);  // read-write so first boot creates the namespace
    values.wifiSsid = prefs.getString("ssid", WIFI_SSID);
    values.wifiPass = prefs.getString("pass", WIFI_PASSWORD);
    values.sessionName = prefs.getString("name", RTPMIDI_SESSION_NAME);
    values.targetIp = prefs.getString("tip", "");
    values.targetPort = prefs.getUShort("tport", 5004);
    prefs.end();
}

bool Config::save(const Values& v) {
    if (!prefs.begin("midicfg", false)) return false;
    prefs.putString("ssid", v.wifiSsid);
    prefs.putString("pass", v.wifiPass);
    prefs.putString("name", v.sessionName);
    prefs.putString("tip", v.targetIp);
    prefs.putUShort("tport", v.targetPort);
    prefs.end();
    values = v;
    return true;
}

const Config::Values& Config::get() {
    return values;
}
