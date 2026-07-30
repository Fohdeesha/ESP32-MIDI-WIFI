#pragma once

#include <WString.h>

#include <cstdint>

namespace UsbMidi {
// One alternate setting of one MIDIStreaming interface, exactly as the
// attached device presents it. inCables/outCables are bNumEmbMIDIJack from
// the class-specific MS_GENERAL endpoint descriptors -- i.e. how many virtual
// cables the device claims in each direction; 0 when it omits the descriptor
// (common on cheap devices, which then still use cable 0).
struct IfaceInfo {
    uint8_t num;
    uint8_t alt;
    uint8_t epIn;   // 0 = this interface sends nothing host-ward
    uint8_t epOut;  // 0 = this interface accepts nothing device-ward
    uint16_t epInMps;
    uint16_t epOutMps;
    uint8_t inCables;
    uint8_t outCables;
};

// Installs the ESP-IDF usb_host stack and starts its event tasks. The OTG
// port (GPIO19/20) becomes host-only from this point -- console must be on
// UART0. Call once from setup(). preferredInterface is the bInterfaceNumber
// to claim on attach (Config::IFACE_AUTO = whichever MIDIStreaming interface
// comes first); an absent one falls back to auto rather than not bridging.
void begin(uint8_t preferredInterface);
// Pops one 4-byte USB-MIDI event packet off the receive queue (and logs it
// to serial + the web UI's event ring). Returns false when the queue is
// empty. Call from loop context only.
bool readPacket(uint8_t out[4]);
// Queues one 4-byte USB-MIDI event packet for transmission to the device.
// Waits up to 20 ms for queue space (burst backpressure); returns false when
// no device is attached or the queue stayed full.
bool writePacket(const uint8_t pkt[4]);
uint32_t txPacketCount();  // packets confirmed delivered on the wire
uint32_t txDropCount();    // packets lost to a full TX queue despite the wait
bool deviceConnected();
// True while the attached device is demonstrably WORKING, not merely present:
// connected AND the IN pipeline has live transfers (an errored-idle RX pipe is
// a deaf device that still enumerates -- only checked when the claimed
// interface actually has an IN endpoint) AND no OUT transfer has sat unACKed
// for >2 s (a healthy bulk OUT completes in under a millisecond; a wedged
// device controller can NAK forever while still looking attached). This is
// what gates the session health heartbeat -- the bridge must never claim a
// device it cannot actually talk to is alive.
bool healthy();
const char* deviceName();  // product string of the attached device, "" if none
const char* statusText();  // human-readable host state for the web UI
uint32_t eventCount();     // MIDI events received since boot

// --- MIDI function discovery (1.3.0), for the web UI's port picker ---------
// Every MIDIStreaming interface found on the last attached device.
uint8_t ifaceCount();
bool ifaceAt(uint8_t i, IfaceInfo& out);
// bInterfaceNumber currently claimed, 0xFF when nothing is claimed.
uint8_t claimedInterface();
bool claimedInterfaceInfo(IfaceInfo& out);
// True when the configured interface was not present on this device and the
// firmware claimed a different one instead (surfaced on the status page).
bool interfaceFellBack();
// Event packets tagged with this virtual cable since the current attach, so
// the picker can show which cables a device really uses -- descriptors are
// not always honest, observed traffic is.
uint32_t cableRxCount(uint8_t cable);
// Appends the last few decoded events (oldest first), sep between entries.
// Each line is prefixed with the virtual cable the packet is on.
void appendRecentEvents(String& out, const char* sep);
// Same for the device-bound direction, so the cable actually stamped on
// outgoing packets can be read off the status page rather than assumed.
void appendRecentTxEvents(String& out, const char* sep);
uint32_t txFormattedCount();
// Parsed + raw config descriptor of the last attached device ("" if none).
const char* descriptorDump();
// One-line IN-pipeline health: completed transfers, how many came back with a
// FULL buffer (the device had data queued, i.e. it was accumulating between
// polls), the worst submit->complete dwell, the worst complete->resubmit gap,
// and the completion-interval histogram. Distinguishes "the device sent
// nothing" from "the host left the endpoint unpolled" -- the two candidate
// causes of clumped MIDI IN, which look identical from the network side.
void appendRxDiag(String& out);
}  // namespace UsbMidi
