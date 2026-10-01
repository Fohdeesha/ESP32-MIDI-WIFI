#pragma once

#include <IPAddress.h>

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The UDP transport the AppleMIDI session runs on (1.9.1): the part of
// WiFiUDP's interface the library uses, over a plain lwIP socket, with a deep
// receive queue in front that a task of its own keeps filled.
//
// lwIP keeps at most 6 datagrams waiting per UDP socket
// (CONFIG_LWIP_UDP_RECVMBOX_SIZE, compiled into the prebuilt SDK) and drops
// the rest without a trace. The library takes the next datagram only once it
// has handed over every message of the last, and the MIDI task can spend
// milliseconds on one message (a SysEx is dozens of USB packets), so a host
// that sent a burst of datagrams lost its tail. Measured: 8 datagrams sent
// at once, the 8th never arrived; the same 8 sent 50 ms apart all did.
//
// So the sockets are read by a receive task ("udp_rx", core 0, priority 20):
// above lwIP's own thread (18), so it takes each datagram the moment lwIP
// posts it, before lwIP can post a 7th, and below the WiFi driver (23). It
// copies datagrams into a 64-deep queue per socket in PSRAM, and wakes the
// MIDI task, so network input no longer waits for its 1 ms poll either. If
// the task cannot be started, the MIDI task polls the socket itself, as with
// WiFiUDP before.
//
// Sending is WiFiUDP's, byte for byte: a 1460-byte buffer filled by write()
// and sent by endPacket(). Receiving hands over at most 1472 bytes per
// datagram, the most an unfragmented one carries (this lwIP does not
// reassemble fragments; WiFiUDP stopped at 1460).
//
// Everything but the receive task's side belongs to the MIDI task, like the
// session that owns these sockets; the two meet only in the queue indexes,
// under one spinlock.
class BufferedUDP {
public:
    BufferedUDP() = default;
    ~BufferedUDP();
    BufferedUDP(const BufferedUDP&) = delete;
    BufferedUDP& operator=(const BufferedUDP&) = delete;

    uint8_t begin(uint16_t port);  // 1 = bound to port on every interface
    void stop();

    // Makes the oldest queued datagram current and returns its size; 0 when
    // none is waiting, or while the current one still has unread bytes.
    int parsePacket();
    int available();  // unread bytes of the current datagram
    int read();       // one byte of it, -1 at its end
    int read(uint8_t* buf, size_t len);
    IPAddress remoteIP() const { return IPAddress(curIp_); }  // the current datagram's sender
    uint16_t remotePort() const { return curPort_; }
    void flush();  // drops the rest of the current datagram (as WiFiUDP's does)

    int beginPacket(IPAddress ip, uint16_t port);
    size_t write(uint8_t b);
    size_t write(const uint8_t* buf, size_t len);
    int endPacket();

    // The task to wake (xTaskNotifyGive) when datagrams are queued.
    static void setNotify(TaskHandle_t task);
    // True once after each of those notifications: tells the woken task a
    // datagram woke it rather than USB.
    static bool takeNotified();

    // For /diag, since boot.
    static uint32_t queuedMax();   // most datagrams ever waiting in one queue
    static uint32_t fullCount();   // times a full queue left datagrams in lwIP
    static uint32_t slotsEach();   // queue depth per socket (0 = none open yet)
    static bool inPsram();         // where the queues live
    static bool taskRunning();     // false = the MIDI task polls instead
    static uint32_t taskStackFree();  // bytes, lowest since start (0 = no task)

private:
    static constexpr size_t MAX_DATAGRAM = 1472;
    static constexpr size_t TX_BUFFER = 1460;  // WiFiUDP's
    struct Slot {
        uint32_t ip;
        uint16_t port;
        uint16_t len;
        uint8_t data[MAX_DATAGRAM];
    };

    static void rxTask(void*);
    bool pump(bool& full);  // the producer: receive task, or MIDI task without one

    int fd_ = -1;
    Slot* slots_ = nullptr;  // kept until destruction: the receive task may be mid-copy
    uint16_t nSlots_ = 0;
    uint16_t head_ = 0;   // oldest queued, under the lock
    uint16_t count_ = 0;  // queued, under the lock; the current one is not counted
    uint8_t* cur_ = nullptr;  // the current datagram, copied out of its slot
    uint16_t curLen_ = 0;
    uint16_t curPos_ = 0;
    uint32_t curIp_ = 0;
    uint16_t curPort_ = 0;
    uint8_t* tx_ = nullptr;
    size_t txLen_ = 0;
    uint32_t txIp_ = 0;
    uint16_t txPort_ = 0;
};
