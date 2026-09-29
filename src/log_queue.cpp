#include "log_queue.h"

#include <Arduino.h>
#include <cstdarg>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace {
constexpr int LINES = 16;
constexpr size_t LINE_LEN = 96;

QueueHandle_t s_queue = nullptr;
// More than one task may drop a line, so the count is guarded.
portMUX_TYPE s_dropMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_drops = 0;
}  // namespace

void LogQueue::begin() {
    if (!s_queue) s_queue = xQueueCreate(LINES, LINE_LEN);
}

void LogQueue::printf(const char* fmt, ...) {
    char line[LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!s_queue || xQueueSend(s_queue, line, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_dropMux);
        s_drops++;
        portEXIT_CRITICAL(&s_dropMux);
    }
}

void LogQueue::drain() {
    if (!s_queue) return;
    char line[LINE_LEN];
    while (xQueueReceive(s_queue, line, 0) == pdTRUE) Serial.println(line);
}

uint32_t LogQueue::drops() {
    return s_drops;
}
