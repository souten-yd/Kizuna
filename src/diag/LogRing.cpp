#include "LogRing.hpp"

#include <esp_attr.h>
#include <esp_log.h>

namespace appdiag {
namespace {

constexpr size_t kRingBytes = 4096;
constexpr size_t kRtcBytes = 1024;
constexpr uint32_t kRtcMagic = 0x4d354c47;  // 'M5LG'

char g_ring[kRingBytes];
size_t g_head = 0;
uint32_t g_total = 0;

// esp_log is called from every task; the ring is written under a spinlock
// rather than a mutex because a logging call must never block or yield - the
// audio task logs a dropped chunk from inside its own timing budget.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t g_chain = nullptr;
bool g_ready = false;

// Not zeroed by a reset, only by a power cycle. RTC_NOINIT_ATTR is the whole
// mechanism here: the linker puts these in RTC slow memory and the startup
// code leaves them alone.
RTC_NOINIT_ATTR uint32_t g_rtcMagic;
RTC_NOINIT_ATTR uint32_t g_rtcHead;
RTC_NOINIT_ATTR uint32_t g_rtcLen;
RTC_NOINIT_ATTR char g_rtcRing[kRtcBytes];

char g_prevTail[kRtcBytes + 1];

void rtcPut(const char* data, size_t len) {
    if (len >= kRtcBytes) {
        data += len - kRtcBytes + 1;
        len = kRtcBytes - 1;
    }
    for (size_t i = 0; i < len; ++i) {
        g_rtcRing[g_rtcHead] = data[i];
        g_rtcHead = (g_rtcHead + 1) % kRtcBytes;
    }
    if (g_rtcLen < kRtcBytes) {
        g_rtcLen = g_rtcLen + len > kRtcBytes ? kRtcBytes : g_rtcLen + len;
    }
}

void put(const char* data, size_t len) {
    portENTER_CRITICAL(&g_mux);
    for (size_t i = 0; i < len; ++i) {
        g_ring[g_head] = data[i];
        g_head = (g_head + 1) % kRingBytes;
    }
    g_total += len;
    rtcPut(data, len);
    portEXIT_CRITICAL(&g_mux);
}

}  // namespace

void LogRing::begin() {
    if (g_ready) return;

    // Lift the previous boot's tail out before this boot starts overwriting
    // it. Oldest first, so it reads in the order it was written.
    if (g_rtcMagic == kRtcMagic && g_rtcLen > 0 && g_rtcLen <= kRtcBytes &&
        g_rtcHead < kRtcBytes) {
        const uint32_t start = (g_rtcHead + kRtcBytes - g_rtcLen) % kRtcBytes;
        for (uint32_t i = 0; i < g_rtcLen; ++i) {
            g_prevTail[i] = g_rtcRing[(start + i) % kRtcBytes];
        }
        g_prevTail[g_rtcLen] = '\0';
    } else {
        g_prevTail[0] = '\0';
    }

    g_rtcMagic = kRtcMagic;
    g_rtcHead = 0;
    g_rtcLen = 0;

    g_ready = true;
    g_chain = esp_log_set_vprintf(&LogRing::hook);

    // The Arduino core sets every tag to ERROR in initArduino(), because the
    // prebuilt IDF is configured that way (CONFIG_LOG_DEFAULT_LEVEL=1). With
    // the firmware's own log_i() now routed through esp_log - see
    // USE_ESP_IDF_LOG in platformio.ini - that filter would throw away every
    // line this ring exists to keep, and throw it away on the serial port too.
    //
    // Only our own tag is lifted. The IDF's drivers stay at ERROR, which is
    // where they were and where they are readable; turning them up to INFO
    // fills four kilobytes of ring with Wi-Fi housekeeping and buries the one
    // line that matters.
    esp_log_level_set("m5", ESP_LOG_INFO);
}

int LogRing::hook(const char* fmt, va_list ap) {
    // The chain consumes its va_list, so format into ours from a copy and
    // hand the original on untouched. Losing the serial log to gain the ring
    // would be a bad trade; both get every line.
    char line[224];
    va_list mine;
    va_copy(mine, ap);
    const int n = vsnprintf(line, sizeof(line), fmt, mine);
    va_end(mine);
    if (n > 0) put(line, n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1);
    return g_chain ? g_chain(fmt, ap) : n;
}

uint32_t LogRing::seq() {
    portENTER_CRITICAL(&g_mux);
    const uint32_t v = g_total;
    portEXIT_CRITICAL(&g_mux);
    return v;
}

String LogRing::since(uint32_t from, uint32_t& firstSeq, uint32_t& upto) {
    portENTER_CRITICAL(&g_mux);
    const uint32_t total = g_total;
    const size_t head = g_head;
    portEXIT_CRITICAL(&g_mux);

    const uint32_t held = total < kRingBytes ? total : kRingBytes;
    const uint32_t oldest = total - held;
    if (from < oldest) from = oldest;
    if (from > total) from = total;
    firstSeq = from;
    upto = total;

    const uint32_t want = total - from;
    String out;
    // Copied outside the lock on purpose. Building a four-kilobyte String
    // inside a critical section means malloc inside a critical section, which
    // is a far worse bug than the one this risks: a line logged by another
    // task mid-copy can garble the oldest bytes of the answer. A diagnostic
    // that is occasionally smudged beats a device that occasionally deadlocks
    // while being asked how it is.
    if (!want) return out;
    out.reserve(want + 1);
    // `head` is where the next byte goes, so the byte with sequence `from`
    // sits `total - from` bytes behind it.
    size_t idx = (head + kRingBytes - want) % kRingBytes;
    for (uint32_t i = 0; i < want; ++i) {
        out += g_ring[idx];
        idx = (idx + 1) % kRingBytes;
    }
    return out;
}

const char* LogRing::previousBootTail() { return g_prevTail; }

}  // namespace appdiag
