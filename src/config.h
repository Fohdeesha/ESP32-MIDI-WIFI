#pragma once

#include <Arduino.h>

namespace Config {
struct Values {
    String wifiSsid;
    String wifiPass;
    String sessionName;  // RTP-MIDI session name shown to peers
    String targetIp;     // AppleMIDI initiator target, "" = listen only
    uint16_t targetPort;
    String webPass;      // web UI password, "" = no protection
    String staticIp;     // "" = DHCP; otherwise station uses this address
    String staticMask;   // subnet mask, only used when staticIp is set
    String staticGw;     // gateway, only used when staticIp is set
    String staticDns;    // DNS server, "" = use gateway (only when staticIp set)
    // Which MIDI function of the attached USB device to bridge. A USB MIDI
    // device can present several MIDIStreaming interfaces, and each interface
    // can carry up to 16 virtual cables (the "ports" a DAW would list).
    uint8_t usbIface;  // bInterfaceNumber to claim; IFACE_AUTO = first usable
    uint8_t usbCable;  // virtual cable to bridge; CABLE_ALL = all, merged
};

constexpr uint8_t IFACE_AUTO = 0xFF;
constexpr uint8_t CABLE_ALL = 0xFF;

// Loads NVS-stored values; anything unset gets a safe default (no baked-in
// credentials -- a blank config boots into the setup AP portal).
void load();
bool save(const Values& v);
const Values& get();
// Factory reset: erases all persisted settings (config + boot guard state).
void wipeAll();
}  // namespace Config
