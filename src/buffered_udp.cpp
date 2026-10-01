#include "buffered_udp.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <lwip/sockets.h>

#include <cstring>

namespace {
// A full control-surface resync is a handful of datagrams and a SysEx dump of
// tens of kB a few dozen, so 64 per socket rides out either while the MIDI
// task works through it. 94 kB each, in PSRAM. Without PSRAM a shallower
// queue in internal RAM still more than doubles what lwIP alone would hold.
constexpr uint16_t SLOTS_PSRAM = 64;
constexpr uint16_t SLOTS_INTERNAL = 8;
constexpr int MAX_OPEN = 4;  // AppleMIDI opens two
constexpr uint32_t RX_STACK = 3072;
constexpr UBaseType_t RX_PRIORITY = 20;  // above lwIP's tcpip thread (18), below WiFi (23)
constexpr BaseType_t RX_CORE = 0;        // where both of those run
// Also how soon a newly opened socket is watched: AppleMIDI opens the data
// port just after the control port, whose begin() starts this task.
constexpr int SELECT_TIMEOUT_MS = 10;

// Guards s_open, s_pumping and every queue's head_/count_. Held only for a
// few loads and stores: datagrams are copied outside it.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
BufferedUDP* s_open[MAX_OPEN] = {};
const BufferedUDP* s_pumping = nullptr;  // the queue the receive task is filling
TaskHandle_t s_rxTask = nullptr;
bool s_rxFailed = false;
TaskHandle_t volatile s_notify = nullptr;
volatile bool s_notified = false;  // set before each notification, cleared by takeNotified()
// Diagnostics: written by whichever task fills the queues, read anywhere.
volatile uint32_t s_queuedMax = 0;
volatile uint32_t s_fullCount = 0;
uint32_t s_slotsEach = 0;
bool s_inPsram = false;

void* allocInternal(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
}  // namespace

BufferedUDP::~BufferedUDP() {
    stop();
    heap_caps_free(slots_);
    heap_caps_free(cur_);
    heap_caps_free(tx_);
}

uint8_t BufferedUDP::begin(uint16_t port) {
    stop();
    // The queue: PSRAM if the board has it, else a short one in internal RAM.
    // Allocated once and kept, so a stop() can never free memory the receive
    // task is still copying into.
    if (!slots_) {
        slots_ = static_cast<Slot*>(
            heap_caps_malloc(SLOTS_PSRAM * sizeof(Slot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        nSlots_ = SLOTS_PSRAM;
        if (!slots_) {
            slots_ = static_cast<Slot*>(allocInternal(SLOTS_INTERNAL * sizeof(Slot)));
            nSlots_ = SLOTS_INTERNAL;
        }
        if (!slots_) nSlots_ = 0;
        s_inPsram = nSlots_ == SLOTS_PSRAM;
        s_slotsEach = nSlots_;
    }
    // The datagram being parsed and the send buffer are touched per byte:
    // internal RAM.
    if (!cur_) cur_ = static_cast<uint8_t*>(allocInternal(MAX_DATAGRAM));
    if (!tx_) tx_ = static_cast<uint8_t*>(allocInternal(TX_BUFFER));
    if (!slots_ || !cur_ || !tx_) return 0;

    // As WiFiUDP::begin(): reusable address, every interface, non-blocking.
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return 0;
    int yes = 1;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return 0;
    }
    fcntl(fd_, F_SETFL, O_NONBLOCK);

    portENTER_CRITICAL(&s_mux);
    for (auto& p : s_open) {
        if (!p) {
            p = this;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (!s_rxTask && !s_rxFailed &&
        xTaskCreatePinnedToCore(rxTask, "udp_rx", RX_STACK, nullptr, RX_PRIORITY, &s_rxTask,
                                RX_CORE) != pdPASS) {
        s_rxTask = nullptr;
        s_rxFailed = true;  // the MIDI task polls in parsePacket() instead
    }
    return 1;
}

void BufferedUDP::stop() {
    portENTER_CRITICAL(&s_mux);
    for (auto& p : s_open) {
        if (p == this) p = nullptr;
    }
    portEXIT_CRITICAL(&s_mux);
    // The receive task may be filling this queue right now: let it finish.
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        const bool busy = s_pumping == this;
        portEXIT_CRITICAL(&s_mux);
        if (!busy) break;
        vTaskDelay(1);
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    portENTER_CRITICAL(&s_mux);
    head_ = count_ = 0;
    portEXIT_CRITICAL(&s_mux);
    curLen_ = curPos_ = 0;
    txLen_ = 0;
}

// Moves every datagram lwIP holds for this socket into the queue, as far as
// it has room. Returns true if it queued any; sets full when it ran out.
bool BufferedUDP::pump(bool& full) {
    bool queued = false;
    if (!slots_ || !nSlots_) return false;
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        const bool isFull = count_ == nSlots_;
        const uint16_t tail = (head_ + count_) % nSlots_;
        portEXIT_CRITICAL(&s_mux);
        if (isFull) {
            // Whatever is still waiting stays with lwIP for now (6 more
            // datagrams before it drops one), and the caller backs off --
            // always, even when FIONREAD sees nothing: an empty datagram
            // keeps the socket readable at 0 bytes, and the receive task
            // would otherwise spin at priority 20 and starve lwIP itself.
            full = true;
            int waiting = 0;
            if (lwip_ioctl(fd_, FIONREAD, &waiting) == 0 && waiting > 0) {
                s_fullCount = s_fullCount + 1;
            }
            return queued;
        }
        // The MIDI task never reads the tail slot: it is not counted yet.
        Slot& s = slots_[tail];
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(fd_, s.data, MAX_DATAGRAM, MSG_DONTWAIT,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) return queued;  // nothing waiting (or a socket error, which WiFiUDP ignored too)
        if (n == 0) continue;      // an empty datagram carries nothing (WiFiUDP skipped them too)
        s.ip = from.sin_addr.s_addr;
        s.port = ntohs(from.sin_port);
        s.len = static_cast<uint16_t>(n);
        portENTER_CRITICAL(&s_mux);
        const uint16_t depth = ++count_;
        portEXIT_CRITICAL(&s_mux);
        if (depth > s_queuedMax) s_queuedMax = depth;
        queued = true;
    }
}

void BufferedUDP::rxTask(void*) {
    for (;;) {
        BufferedUDP* socks[MAX_OPEN];
        int fds[MAX_OPEN];
        int n = 0;
        portENTER_CRITICAL(&s_mux);
        for (BufferedUDP* p : s_open) {
            if (p && p->fd_ >= 0) {
                socks[n] = p;
                fds[n] = p->fd_;
                n++;
            }
        }
        portEXIT_CRITICAL(&s_mux);
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(SELECT_TIMEOUT_MS));
            continue;
        }
        fd_set rd;
        FD_ZERO(&rd);
        int maxFd = -1;
        for (int i = 0; i < n; i++) {
            FD_SET(fds[i], &rd);
            if (fds[i] > maxFd) maxFd = fds[i];
        }
        timeval tv = {0, SELECT_TIMEOUT_MS * 1000};
        const int ready = select(maxFd + 1, &rd, nullptr, nullptr, &tv);
        if (ready < 0) {
            vTaskDelay(1);  // a socket closed under it: look again
            continue;
        }
        if (ready == 0) continue;
        bool queued = false, full = false;
        for (int i = 0; i < n; i++) {
            if (!FD_ISSET(fds[i], &rd)) continue;
            BufferedUDP* p = socks[i];
            portENTER_CRITICAL(&s_mux);
            bool open = false;
            for (BufferedUDP* q : s_open) open |= q == p;
            if (open) s_pumping = p;  // stop() now waits for us
            portEXIT_CRITICAL(&s_mux);
            if (!open) continue;
            queued |= p->pump(full);
            portENTER_CRITICAL(&s_mux);
            s_pumping = nullptr;
            portEXIT_CRITICAL(&s_mux);
        }
        TaskHandle_t notify = s_notify;
        if (queued && notify) {
            s_notified = true;  // before the notification, so the woken task sees it
            xTaskNotifyGive(notify);
        }
        // A socket stays readable while its queue is full: let the MIDI task
        // drain some rather than spin on it. Likewise if select() said
        // readable but nothing could be queued (a socket error that does not
        // clear): never loop at this priority without making progress.
        if (full || !queued) vTaskDelay(1);
    }
}

int BufferedUDP::parsePacket() {
    if (curPos_ < curLen_) return 0;  // as WiFiUDP: the current one is not read out yet
    if (!slots_ || !nSlots_ || !cur_) return 0;  // never begun, or out of memory
    if (!s_rxTask && fd_ >= 0) {
        bool full = false;
        pump(full);
    }
    portENTER_CRITICAL(&s_mux);
    const bool any = count_ > 0;
    const uint16_t head = head_;
    portEXIT_CRITICAL(&s_mux);
    if (!any) {
        curLen_ = curPos_ = 0;
        return 0;
    }
    // The head slot is counted, so the receive task leaves it alone.
    const Slot& s = slots_[head];
    memcpy(cur_, s.data, s.len);
    curLen_ = s.len;
    curPos_ = 0;
    curIp_ = s.ip;
    curPort_ = s.port;
    portENTER_CRITICAL(&s_mux);
    head_ = (head_ + 1) % nSlots_;
    count_--;
    portEXIT_CRITICAL(&s_mux);
    return curLen_;
}

int BufferedUDP::available() {
    return curLen_ - curPos_;
}

int BufferedUDP::read() {
    return curPos_ < curLen_ ? cur_[curPos_++] : -1;
}

int BufferedUDP::read(uint8_t* buf, size_t len) {
    size_t n = curLen_ - curPos_;
    if (n > len) n = len;
    memcpy(buf, cur_ + curPos_, n);
    curPos_ += n;
    return static_cast<int>(n);
}

void BufferedUDP::flush() {
    curPos_ = curLen_;
}

// The send side mirrors WiFiUDP: beginPacket() starts a new datagram (and,
// on a socket never begun, opens an unbound one), write() fills a 1460-byte
// buffer and sends it early when full, endPacket() sends.
int BufferedUDP::beginPacket(IPAddress ip, uint16_t port) {
    txIp_ = static_cast<uint32_t>(ip);
    txPort_ = port;
    if (!port) return 0;
    if (!tx_) {
        tx_ = static_cast<uint8_t*>(allocInternal(TX_BUFFER));
        if (!tx_) return 0;
    }
    txLen_ = 0;
    if (fd_ >= 0) return 1;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return 0;
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    return 1;
}

size_t BufferedUDP::write(uint8_t b) {
    if (!tx_) return 0;
    if (txLen_ == TX_BUFFER) {
        endPacket();
        txLen_ = 0;
    }
    tx_[txLen_++] = b;
    return 1;
}

size_t BufferedUDP::write(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) write(buf[i]);
    return len;
}

int BufferedUDP::endPacket() {
    if (fd_ < 0 || !tx_) return 0;
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(txPort_);
    to.sin_addr.s_addr = txIp_;
    return sendto(fd_, tx_, txLen_, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0 ? 0 : 1;
}

void BufferedUDP::setNotify(TaskHandle_t task) {
    s_notify = task;
}

bool BufferedUDP::takeNotified() {
    if (!s_notified) return false;
    s_notified = false;
    return true;
}

uint32_t BufferedUDP::queuedMax() {
    return s_queuedMax;
}

uint32_t BufferedUDP::fullCount() {
    return s_fullCount;
}

uint32_t BufferedUDP::slotsEach() {
    return s_slotsEach;
}

bool BufferedUDP::inPsram() {
    return s_inPsram;
}

bool BufferedUDP::taskRunning() {
    return s_rxTask != nullptr;
}

uint32_t BufferedUDP::taskStackFree() {
    return s_rxTask ? uxTaskGetStackHighWaterMark(s_rxTask) : 0;
}
