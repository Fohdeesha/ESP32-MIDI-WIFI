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
    uint8_t usbCable;  // device->network cable; CABLE_ALL = all, merged
    // network->device cable. Its own setting because a device's in and out
    // cable counts are independent in the descriptors, and "all ports in" has
    // no single cable for the return path to follow. CABLE_SAME (the default)
    // keeps the two directions locked together, which is what a control
    // surface needs -- it expects its LEDs back on the port it sent from.
    uint8_t usbCableOut;
    // WiFi TX power, stored as the wifi_power_t raw value (quarter-dBm: 34 =
    // 8.5 dBm). Capped at 11 dBm since 1.7.3: on boards with a weak 3.3 V
    // regulator the transmit current sags the rail, and above ~8.5 dBm that
    // collapsed the link under a steady MIDI stream (and at 19.5 dBm browned
    // out the board's USB-serial chip). Only values in TX_POWER_CHOICES are
    // accepted.
    uint8_t txPower;
};

constexpr uint8_t IFACE_AUTO = 0xFF;
constexpr uint8_t CABLE_ALL = 0xFF;   // input only
constexpr uint8_t CABLE_SAME = 0xFE;  // output only: follow the input setting
// Allowed TX power settings (wifi_power_t raw values): 11 / 8.5 / 7 / 5 / 2 dBm.
constexpr uint8_t TX_POWER_CHOICES[] = {44, 34, 28, 20, 8};
constexpr uint8_t TX_POWER_DEFAULT = 34;  // 8.5 dBm
inline bool txPowerValid(uint8_t v) {
    for (uint8_t c : TX_POWER_CHOICES)
        if (v == c) return true;
    return false;
}

// Loads NVS-stored values; anything unset gets a safe default (no baked-in
// credentials -- a blank config boots into the setup AP portal).
void load();
bool save(const Values& v);
const Values& get();
// Factory reset: erases all persisted settings (config + boot guard state).
void wipeAll();
}  // namespace Config
