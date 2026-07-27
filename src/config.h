#pragma once

#include <Arduino.h>

namespace Config {
struct Values {
    String wifiSsid;
    String wifiPass;
    String sessionName;  // RTP-MIDI session name shown to peers
    String targetIp;     // AppleMIDI initiator target, "" = listen only (future use)
    uint16_t targetPort;
    String webPass;      // web UI password, "" = no protection
};

// Loads NVS-stored values, falling back to secrets.h defaults for anything unset.
void load();
bool save(const Values& v);
const Values& get();
// Factory reset: erases all persisted settings (config + boot guard state).
void wipeAll();
}  // namespace Config
