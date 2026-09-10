#ifdef CLAWDMETER_AUDIO_TEST
#include "audio_test.h"
#include "hal/audio_capture_hal.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <stdarg.h>

#if !ARDUINO_USB_MODE
#error "Audio test requires the existing USB-Serial/JTAG (HWCDC) transport"
#endif

namespace {
constexpr size_t kSamples = 160000;
constexpr size_t kBytes = kSamples * sizeof(int16_t);
constexpr size_t kChunk = 192;
std::atomic<bool> busy{false};
std::atomic<bool> cancelled{false};
std::atomic<uint32_t> loop_count{0};
std::atomic<uint32_t> max_loop_gap{0};
struct Request { unsigned long id; unsigned mic; };

// One bounded HWCDC::write per complete packet. HWCDC's own TX mutex makes
// packets atomic relative to existing Serial writers. Leading newline recovers
// framing after partial log lines. Hex payload cannot contain line delimiters.
bool packet(const char* format, ...) {
    char line[768];
    memcpy(line, "\nAUDIO1 ", 8);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line + 8, sizeof(line) - 10, format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(line) - 10 || !Serial) return false;
    line[n + 8] = '\n';
    return Serial.write((const uint8_t*)line, n + 9) == (size_t)n + 9;
}

uint32_t crc32(const uint8_t* bytes, size_t count) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void memory(unsigned long id, const char* phase) {
    packet("{\"id\":\"%08lx\",\"type\":\"memory\",\"phase\":\"%s\","
           "\"internal\":%u,\"largest\":%u,\"psram\":%u}", id, phase,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void worker(void* arg) {
    const Request request = *(Request*)arg;
    free(arg);
    const unsigned long id = request.id;
    const char* error = nullptr;
    memory(id, "before");
    int16_t* pcm = (int16_t*)heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t count = 0;
    uint32_t capture_ms = 0;
    uint32_t overflows = 0;
    unsigned read_errors = 0;
    bool started = false;
    if (!pcm) error = "psram_allocation";
    else if (!(started = audio_capture_hal_begin(request.mic))) error = "microphone_setup";
    if (!error) {
        memory(id, "capturing");
        if (!packet("{\"id\":\"%08lx\",\"type\":\"start\",\"mic\":%u,"
                    "\"gain_db\":37.5,\"seconds\":10}", id, request.mic)) error = "usb_start";
        uint32_t began = millis();
        const uint32_t initial_loops = loop_count;
        max_loop_gap = 0;
        while (!error && count < kSamples) {
            if (cancelled || !Serial) { error = "cancelled_or_disconnected"; break; }
            if (millis() - began > 12000) { error = "capture_timeout"; break; }
            size_t got = 0;
            if (!audio_capture_hal_read(pcm + count, kSamples - count, &got)) {
                ++read_errors;
                error = "i2s_read";
                break;
            }
            count += got;
        }
        capture_ms = millis() - began;
        overflows = audio_capture_hal_overruns();
        if (overflows) error = "i2s_overrun";
        packet("{\"id\":\"%08lx\",\"type\":\"health\",\"ui_loops\":%lu,\"max_loop_gap_ms\":%lu}",
               id, (unsigned long)(loop_count.load() - initial_loops),
               (unsigned long)max_loop_gap.load());
    }
    // Always stop the microphone BEFORE checksumming or transferring the file.
    if (started && !audio_capture_hal_end()) error = "microphone_cleanup";
    if (!error && (capture_ms < 9500 || capture_ms > 11000)) error = "capture_timing";
    if (!error) {
        const uint32_t crc = crc32((uint8_t*)pcm, kBytes);
        if (!packet("{\"id\":\"%08lx\",\"type\":\"begin\",\"version\":1,"
                    "\"rate\":16000,\"channels\":1,\"bits\":16,\"samples\":160000,"
                    "\"bytes\":320000,\"crc32\":%lu,\"capture_ms\":%lu,"
                    "\"overruns\":%lu,\"read_errors\":%u}",
                    id, (unsigned long)crc, (unsigned long)capture_ms,
                    (unsigned long)overflows, read_errors)) error = "usb_header";
        const char* hex = "0123456789abcdef";
        char encoded[kChunk * 2 + 1];
        const uint32_t began = millis();
        for (size_t offset = 0; !error && offset < kBytes; offset += kChunk) {
            if (cancelled || !Serial || millis() - began > 30000) {
                error = "transfer_cancelled_or_timeout";
                break;
            }
            const size_t size = min(kChunk, kBytes - offset);
            const uint8_t* data = (uint8_t*)pcm + offset;
            for (size_t i = 0; i < size; ++i) {
                encoded[i * 2] = hex[data[i] >> 4];
                encoded[i * 2 + 1] = hex[data[i] & 15];
            }
            encoded[size * 2] = 0;
            if (!packet("{\"id\":\"%08lx\",\"type\":\"data\",\"offset\":%u,\"hex\":\"%s\"}",
                        id, (unsigned)offset, encoded)) error = "usb_data";
            vTaskDelay(1);
        }
        if (!error && !packet("{\"id\":\"%08lx\",\"type\":\"end\",\"crc32\":%lu}",
                              id, (unsigned long)crc)) error = "usb_end";
    }
    heap_caps_free(pcm);
    memory(id, "released");
    if (error) packet("{\"id\":\"%08lx\",\"type\":\"error\",\"reason\":\"%s\"}", id, error);
    busy = false;
    vTaskDelete(nullptr);
}
}

void audio_test_init() {
    // Bound diagnostic writers too; no permanent lock around a ten-second capture.
    Serial.setTxTimeoutMs(20);
    Serial.setTxBufferSize(2048);
}

void audio_test_tick() {
    static uint32_t last = millis();
    uint32_t now = millis();
    uint32_t gap = now - last;
    last = now;
    ++loop_count;
    uint32_t previous = max_loop_gap;
    while (gap > previous && !max_loop_gap.compare_exchange_weak(previous, gap)) {}
}

bool audio_test_command(const char* command) {
    if (!strcmp(command, "audio_info")) {
        packet("{\"type\":\"info\",\"version\":1,\"busy\":%s,\"seconds\":10}",
               busy ? "true" : "false");
        return true;
    }
    if (!strcmp(command, "audio_cancel")) {
        cancelled = true;
        return true;
    }
    if (busy && !strcmp(command, "screenshot")) {
        Serial.println("SCREENSHOT_BUSY_AUDIO");
        return true;
    }
    if (strncmp(command, "audio_record", 12)) return false;
    unsigned long id = 0;
    unsigned mic = 0;
    char extra;
    if (sscanf(command, "audio_record %8lx %u %c", &id, &mic, &extra) != 2 ||
        (mic != 1 && mic != 2)) {
        packet("{\"type\":\"error\",\"reason\":\"invalid_request\"}");
        return true;
    }
    if (busy.exchange(true)) {
        packet("{\"id\":\"%08lx\",\"type\":\"error\",\"reason\":\"busy\"}", id);
        return true;
    }
    cancelled = false;
    Request* request = (Request*)malloc(sizeof(Request));
    if (request) *request = {id, mic};
    if (!request || xTaskCreate(worker, "audio-test", 8192, request, 2, nullptr) != pdPASS) {
        free(request);
        busy = false;
        packet("{\"id\":\"%08lx\",\"type\":\"error\",\"reason\":\"task_allocation\"}", id);
    }
    return true;
}
#endif
