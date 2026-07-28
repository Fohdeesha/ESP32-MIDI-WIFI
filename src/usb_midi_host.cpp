#include "usb_midi_host.h"

#include <Arduino.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

// USB MIDI 1.0: MIDIStreaming interface is Audio class (1), subclass 3.
// Data moves as 4-byte "USB-MIDI event packets" on a bulk endpoint:
// byte 0 = cable number (high nibble) + Code Index Number, bytes 1-3 = MIDI.
namespace {

constexpr uint8_t USB_CLASS_AUDIO_ = 0x01;
constexpr uint8_t USB_SUBCLASS_MIDI_STREAMING = 0x03;

constexpr int NUM_RX_TRANSFERS = 2;  // keep one IN transfer always pending

struct MidiPacket {
    uint8_t b[4];
};

usb_host_client_handle_t s_client = nullptr;
usb_device_handle_t s_device = nullptr;
QueueHandle_t s_queue = nullptr;

uint8_t s_ifaceNum = 0;
uint8_t s_ifaceAlt = 0;
uint8_t s_epIn = 0;  // IN endpoint address; 0 = no MIDI interface found
usb_transfer_t* s_xfers[NUM_RX_TRANSFERS] = {};
volatile int s_inFlight = 0;

// Flags set in the client event callback, acted on in the client task loop
// (descriptor walking + claiming shouldn't run inside the callback).
volatile uint8_t s_pendingAddr = 0;
volatile bool s_pendingGone = false;
volatile bool s_connected = false;
char s_productName[64] = "";
char s_statusText[96] = "not started";

// Raw + parsed config descriptor of the last attached device, for the web
// UI's debug view. Filled once per attach, before s_connected flips.
char s_descDump[4096] = "";

void dumpDescriptors(const usb_config_desc_t* cfg) {
    s_descDump[0] = '\0';
    if (!cfg) return;
    size_t off = 0;
    const uint8_t* p = (const uint8_t*)cfg;
    const uint8_t* end = p + cfg->wTotalLength;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end && off < sizeof(s_descDump) - 64) {
        uint8_t len = p[0], type = p[1];
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* id = (const usb_intf_desc_t*)p;
            off += snprintf(s_descDump + off, sizeof(s_descDump) - off,
                            "if %u alt %u class %02x/%02x proto %02x eps %u\n",
                            id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass,
                            id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints);
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* ed = (const usb_ep_desc_t*)p;
            off += snprintf(s_descDump + off, sizeof(s_descDump) - off,
                            "  ep %02x attr %02x mps %u interval %u\n", ed->bEndpointAddress,
                            ed->bmAttributes, ed->wMaxPacketSize, ed->bInterval);
        } else {
            off += snprintf(s_descDump + off, sizeof(s_descDump) - off, "  type %02x:", type);
            for (int i = 0; i < len && off < sizeof(s_descDump) - 8; i++) {
                off += snprintf(s_descDump + off, sizeof(s_descDump) - off, " %02x", p[i]);
            }
            off += snprintf(s_descDump + off, sizeof(s_descDump) - off, "\n");
        }
        if (off >= sizeof(s_descDump)) off = sizeof(s_descDump) - 1;
        p += len;
    }
}

void utf16ToAscii(const usb_str_desc_t* sd, char* out, size_t outLen) {
    size_t n = 0;
    if (sd) {
        size_t chars = (sd->bLength - 2) / 2;
        for (size_t i = 0; i < chars && n < outLen - 1; i++) {
            uint16_t c = sd->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

void onTransferDone(usb_transfer_t* xfer) {
    s_inFlight--;
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && s_connected) {
        for (int i = 0; i + 3 < xfer->actual_num_bytes; i += 4) {
            const uint8_t* p = &xfer->data_buffer[i];
            if ((p[0] & 0x0F) == 0) continue;  // CIN 0 = reserved/padding
            MidiPacket pkt;
            memcpy(pkt.b, p, 4);
            xQueueSend(s_queue, &pkt, 0);  // full queue: drop, never block
        }
        if (usb_host_transfer_submit(xfer) == ESP_OK) s_inFlight++;
    }
    // Any error status (NO_DEVICE on unplug, stall, ...): leave it idle;
    // cleanup or the next device claim will resubmit.
}

// Walks the active config descriptor for the first MIDIStreaming interface
// and its IN endpoint. Returns false if the device has no MIDI function.
bool findMidiInterface() {
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(s_device, &cfg) != ESP_OK) return false;

    const uint8_t* p = (const uint8_t*)cfg;
    const uint8_t* end = p + cfg->wTotalLength;
    bool inMidiIface = false;
    s_epIn = 0;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* id = (const usb_intf_desc_t*)p;
            inMidiIface = (id->bInterfaceClass == USB_CLASS_AUDIO_ &&
                           id->bInterfaceSubClass == USB_SUBCLASS_MIDI_STREAMING);
            if (inMidiIface && s_epIn == 0) {
                s_ifaceNum = id->bInterfaceNumber;
                s_ifaceAlt = id->bAlternateSetting;
            }
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && inMidiIface && s_epIn == 0) {
            const usb_ep_desc_t* ed = (const usb_ep_desc_t*)p;
            uint8_t type = ed->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            if ((ed->bEndpointAddress & 0x80) &&
                (type == USB_BM_ATTRIBUTES_XFER_BULK || type == USB_BM_ATTRIBUTES_XFER_INT)) {
                s_epIn = ed->bEndpointAddress;
                for (int i = 0; i < NUM_RX_TRANSFERS; i++) {
                    if (!s_xfers[i]) usb_host_transfer_alloc(ed->wMaxPacketSize, 0, &s_xfers[i]);
                    s_xfers[i]->device_handle = s_device;
                    s_xfers[i]->bEndpointAddress = s_epIn;
                    s_xfers[i]->num_bytes = ed->wMaxPacketSize;
                    s_xfers[i]->callback = onTransferDone;
                    s_xfers[i]->context = nullptr;
                }
            }
        }
        p += p[0];
    }
    return s_epIn != 0;
}

// The P1-M reports its high-speed bulk MPS (512) even when enumerated at
// full speed (non-compliant; a FS device must report <= 64). The S3 host is
// FS-only, so pipe allocation rejects the claim with ESP_ERR_NOT_SUPPORTED.
// Clamp the stack's cached descriptor to the FS maximum -- at FS the device
// can't put more than 64 bytes in a packet regardless of what it declares.
void clampFullSpeedMps(const usb_config_desc_t* cfg) {
    if (!cfg) return;
    const uint8_t* p = (const uint8_t*)cfg;
    const uint8_t* end = p + cfg->wTotalLength;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            usb_ep_desc_t* ed = (usb_ep_desc_t*)const_cast<uint8_t*>(p);
            if (ed->wMaxPacketSize > 64) {
                Serial.printf("[usb] ep %02x: clamping bogus MPS %u -> 64\n",
                              ed->bEndpointAddress, ed->wMaxPacketSize);
                ed->wMaxPacketSize = 64;
            }
        }
        p += p[0];
    }
}

void attachDevice(uint8_t addr) {
    if (s_device) return;  // single-device bridge; ignore extra plugs
    if (usb_host_device_open(s_client, addr, &s_device) != ESP_OK) {
        Serial.println("[usb] device_open failed");
        snprintf(s_statusText, sizeof(s_statusText), "device seen but open failed");
        s_device = nullptr;
        return;
    }
    usb_device_info_t info;
    const usb_device_desc_t* dd = nullptr;
    usb_host_device_info(s_device, &info);
    usb_host_get_device_descriptor(s_device, &dd);
    utf16ToAscii(info.str_desc_product, s_productName, sizeof(s_productName));
    Serial.printf("[usb] device at addr %u: VID %04x PID %04x \"%s\"\n", addr,
                  dd ? dd->idVendor : 0, dd ? dd->idProduct : 0, s_productName);
    const usb_config_desc_t* cfg = nullptr;
    usb_host_get_active_config_descriptor(s_device, &cfg);
    dumpDescriptors(cfg);       // dump shows the device's original values
    clampFullSpeedMps(cfg);     // ...then sanitize before claiming

    if (!findMidiInterface()) {
        Serial.println("[usb] no MIDIStreaming interface -- not a USB MIDI device?");
        snprintf(s_statusText, sizeof(s_statusText), "\"%s\" (%04x:%04x): no MIDI interface",
                 s_productName, dd ? dd->idVendor : 0, dd ? dd->idProduct : 0);
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        s_productName[0] = '\0';
        return;
    }
    esp_err_t cerr = usb_host_interface_claim(s_client, s_device, s_ifaceNum, s_ifaceAlt);
    if (cerr != ESP_OK && s_ifaceAlt != 0) {
        cerr = usb_host_interface_claim(s_client, s_device, s_ifaceNum, 0);
    }
    if (cerr != ESP_OK) {
        Serial.printf("[usb] interface_claim failed: %s\n", esp_err_to_name(cerr));
        snprintf(s_statusText, sizeof(s_statusText), "\"%s\": claim if %u alt %u failed: %s",
                 s_productName, s_ifaceNum, s_ifaceAlt, esp_err_to_name(cerr));
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        s_productName[0] = '\0';
        return;
    }
    s_connected = true;
    snprintf(s_statusText, sizeof(s_statusText), "connected: \"%s\"", s_productName);
    for (int i = 0; i < NUM_RX_TRANSFERS; i++) {
        if (usb_host_transfer_submit(s_xfers[i]) == ESP_OK) s_inFlight++;
    }
    Serial.printf("[usb] MIDI interface %u claimed, IN ep 0x%02x -- listening\n", s_ifaceNum,
                  s_epIn);
}

void detachDevice() {
    s_connected = false;
    // Let in-flight transfers drain (they complete with NO_DEVICE) before
    // releasing -- their callbacks arrive through handle_events.
    for (int i = 0; i < 20 && s_inFlight > 0; i++) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
    }
    if (s_device) {
        usb_host_interface_release(s_client, s_device, s_ifaceNum);
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
    }
    for (int i = 0; i < NUM_RX_TRANSFERS; i++) {
        if (s_xfers[i]) {
            usb_host_transfer_free(s_xfers[i]);
            s_xfers[i] = nullptr;
        }
    }
    s_inFlight = 0;
    s_epIn = 0;
    s_productName[0] = '\0';
    snprintf(s_statusText, sizeof(s_statusText), "host active, no device");
    Serial.println("[usb] device disconnected");
}

void onClientEvent(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_pendingAddr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (msg->dev_gone.dev_hdl == s_device) s_pendingGone = true;
    }
}

void clientTask(void*) {
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        if (s_pendingGone) {
            s_pendingGone = false;
            detachDevice();
        }
        if (s_pendingAddr) {
            uint8_t addr = s_pendingAddr;
            s_pendingAddr = 0;
            attachDevice(addr);
        }
    }
}

void daemonTask(void*) {
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// Recent decoded events, kept for the web UI (serial may be unplugged when
// the board is deployed at the instrument). Written and read only from the
// loop task, so no locking.
constexpr int LOG_RING = 12;
char s_ring[LOG_RING][48];
uint32_t s_eventCount = 0;

// Formats one decoded event into buf; returns false for events not worth
// showing (realtime clock/active-sense spam, reserved CINs).
bool formatPacket(const MidiPacket& p, char* buf, size_t len) {
    uint8_t cable = p.b[0] >> 4;
    uint8_t cin = p.b[0] & 0x0F;
    uint8_t ch = (p.b[1] & 0x0F) + 1;
    char pre[8] = "";
    if (cable) snprintf(pre, sizeof(pre), "c%u ", cable);
    switch (cin) {
        case 0x8:
            snprintf(buf, len, "%sch%u note off %u vel %u", pre, ch, p.b[2], p.b[3]);
            return true;
        case 0x9:
            snprintf(buf, len, "%sch%u note on %u vel %u", pre, ch, p.b[2], p.b[3]);
            return true;
        case 0xA:
            snprintf(buf, len, "%sch%u poly AT %u = %u", pre, ch, p.b[2], p.b[3]);
            return true;
        case 0xB:
            snprintf(buf, len, "%sch%u cc %u = %u", pre, ch, p.b[2], p.b[3]);
            return true;
        case 0xC:
            snprintf(buf, len, "%sch%u program %u", pre, ch, p.b[2]);
            return true;
        case 0xD:
            snprintf(buf, len, "%sch%u pressure %u", pre, ch, p.b[2]);
            return true;
        case 0xE:
            snprintf(buf, len, "%sch%u pitch %+d", pre, ch, (p.b[2] | (p.b[3] << 7)) - 8192);
            return true;
        case 0xF:
            if (p.b[1] == 0xF8 || p.b[1] == 0xFE) return false;
            snprintf(buf, len, "%srealtime %02x", pre, p.b[1]);
            return true;
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
            snprintf(buf, len, "%ssys/sysex %02x %02x %02x", pre, p.b[1], p.b[2], p.b[3]);
            return true;
        default:
            return false;
    }
}

}  // namespace

void UsbMidi::begin() {
    s_queue = xQueueCreate(128, sizeof(MidiPacket));

    usb_host_config_t hostCfg = {};
    hostCfg.skip_phy_setup = false;
    hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hostCfg);
    if (err != ESP_OK) {
        Serial.printf("[usb] host install failed: %s\n", esp_err_to_name(err));
        snprintf(s_statusText, sizeof(s_statusText), "host install failed: %s",
                 esp_err_to_name(err));
        return;
    }

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous = false;
    clientCfg.max_num_event_msg = 5;
    clientCfg.async.client_event_callback = onClientEvent;
    clientCfg.async.callback_arg = nullptr;
    err = usb_host_client_register(&clientCfg, &s_client);
    if (err != ESP_OK) {
        Serial.printf("[usb] client register failed: %s\n", esp_err_to_name(err));
        snprintf(s_statusText, sizeof(s_statusText), "client register failed: %s",
                 esp_err_to_name(err));
        return;
    }

    // Above loopTask (prio 1) so MIDI IN transfers get serviced promptly,
    // on core 1 to stay clear of the WiFi stack on core 0.
    xTaskCreatePinnedToCore(daemonTask, "usbh_daemon", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(clientTask, "usbh_client", 4096, nullptr, 5, nullptr, 1);
    snprintf(s_statusText, sizeof(s_statusText), "host active, no device");
    Serial.println("[usb] host mode active, waiting for device on OTG port");
}

bool UsbMidi::readPacket(uint8_t out[4]) {
    if (!s_queue) return false;
    MidiPacket pkt;
    if (xQueueReceive(s_queue, &pkt, 0) != pdTRUE) return false;
    char* slot = s_ring[s_eventCount % LOG_RING];
    if (formatPacket(pkt, slot, sizeof(s_ring[0]))) {
        Serial.printf("[usb] %s\n", slot);
        s_eventCount++;
    }
    memcpy(out, pkt.b, 4);
    return true;
}

bool UsbMidi::deviceConnected() {
    return s_connected;
}

const char* UsbMidi::deviceName() {
    return s_productName;
}

const char* UsbMidi::statusText() {
    return s_statusText;
}

uint32_t UsbMidi::eventCount() {
    return s_eventCount;
}

const char* UsbMidi::descriptorDump() {
    return s_descDump;
}

void UsbMidi::appendRecentEvents(String& out, const char* sep) {
    uint32_t n = s_eventCount < LOG_RING ? s_eventCount : LOG_RING;
    for (uint32_t i = 0; i < n; i++) {  // oldest first
        if (i) out += sep;
        out += s_ring[(s_eventCount - n + i) % LOG_RING];
    }
}
