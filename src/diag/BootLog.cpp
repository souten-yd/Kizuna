#include "BootLog.hpp"

#include <Preferences.h>
#include <esp_system.h>

#include "ResetReason.hpp"

namespace appdiag {
namespace {

constexpr const char* kNamespace = "m5boot";
constexpr uint8_t kSlots = 8;
// A minute of resolution is enough to tell "died after two minutes" from "died
// after four hours", and it is what keeps the flash writes down: one record is
// 20 bytes, an NVS page holds a few hundred of those before it has to be
// erased, so the part sees an erase every couple of hours rather than an
// erase a minute.
constexpr uint32_t kHeartbeatMs = 60000;

#pragma pack(push, 1)
struct Record {
    uint32_t index;        // which boot this was, counting from the first ever
    uint32_t uptimeMs;     // the last thing it said before it stopped saying anything
    uint32_t minHeap;      // low water mark, because a slow leak ends as a reboot
    uint8_t reason;        // esp_reset_reason() at the start of this boot
    uint8_t battery;
    uint8_t charging;
    uint8_t clean;         // 1 when the firmware asked for the restart itself
};
#pragma pack(pop)

Preferences g_prefs;
bool g_ready = false;
uint8_t g_crashStreak = 0;
Record g_current{};
Record g_previous{};
bool g_hasPrevious = false;
uint8_t g_slot = 0;
uint32_t g_lastFlushMs = 0;
char g_key[8];

const char* slotKey(uint8_t slot) {
    snprintf(g_key, sizeof(g_key), "r%u", slot);
    return g_key;
}

// A boot that began because the last one fell over, as opposed to one that
// began because someone reached for the plug.
bool startedAfterCrash(uint8_t reason) {
    switch (static_cast<esp_reset_reason_t>(reason)) {
        case ESP_RST_PANIC:
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

const char* reasonName(uint8_t reason) {
    return appdiag::resetReasonName(static_cast<esp_reset_reason_t>(reason));
}

void appendRecord(String& out, const Record& r) {
    out += "{\"boot\":";
    out += r.index;
    out += ",\"reason\":\"";
    out += reasonName(r.reason);
    out += "\",\"uptime_ms\":";
    out += r.uptimeMs;
    out += ",\"min_heap\":";
    out += r.minHeap;
    out += ",\"battery\":";
    out += r.battery;
    out += ",\"charging\":";
    out += r.charging ? "true" : "false";
    out += ",\"clean\":";
    out += r.clean ? "true" : "false";
    out += '}';
}

}  // namespace

void BootLog::begin() {
    if (g_ready) return;
    g_ready = g_prefs.begin(kNamespace, false);
    if (!g_ready) {
        log_w("boot log unavailable");
        return;
    }

    const uint32_t count = g_prefs.getUInt("n", 0);
    if (count) {
        const uint8_t prevSlot = static_cast<uint8_t>((count - 1) % kSlots);
        g_hasPrevious =
            g_prefs.getBytes(slotKey(prevSlot), &g_previous, sizeof(g_previous)) ==
            sizeof(g_previous);
    }

    g_current = Record{};
    g_current.index = count + 1;
    g_current.reason = static_cast<uint8_t>(esp_reset_reason());
    g_slot = static_cast<uint8_t>(count % kSlots);
    g_prefs.putUInt("n", g_current.index);
    flush(true);

    // Walk back through the ring counting boots that began after a crash.
    // Stops at the first that did not, which is what makes it a streak.
    g_crashStreak = 0;
    if (startedAfterCrash(g_current.reason)) {
        g_crashStreak = 1;
        for (uint32_t i = count; i >= 1 && g_crashStreak < kSlots; --i) {
            Record r{};
            if (g_prefs.getBytes(slotKey(static_cast<uint8_t>((i - 1) % kSlots)), &r,
                                 sizeof(r)) != sizeof(r)) {
                break;
            }
            if (r.index != i || !startedAfterCrash(r.reason)) break;
            ++g_crashStreak;
        }
        log_w("boot log: %u boots in a row began after a crash", (unsigned)g_crashStreak);
    }

    if (g_hasPrevious) {
        // Said out loud on every start, because this is the line that is worth
        // reading first and it should not need a request to appear.
        log_i("previous boot #%u ran %u ms, ended %s, battery %u%%%s",
              (unsigned)g_previous.index, (unsigned)g_previous.uptimeMs,
              g_previous.clean ? "on purpose" : "without warning", g_previous.battery,
              g_previous.charging ? " (charging)" : " (on battery)");
    }
    log_i("boot #%u reason=%s", (unsigned)g_current.index, reasonName(g_current.reason));
}

void BootLog::flush(bool force) {
    if (!g_ready) return;
    g_prefs.putBytes(slotKey(g_slot), &g_current, sizeof(g_current));
    (void)force;
}

void BootLog::heartbeat(uint32_t nowMs, uint8_t battery, bool charging) {
    if (!g_ready) return;
    if (g_lastFlushMs && nowMs - g_lastFlushMs < kHeartbeatMs) return;
    g_lastFlushMs = nowMs ? nowMs : 1;
    g_current.uptimeMs = nowMs;
    g_current.battery = battery;
    g_current.charging = charging ? 1 : 0;
    g_current.minHeap = ESP.getMinFreeHeap();
    flush(true);
}

void BootLog::noteCleanShutdown(const char* why) {
    if (!g_ready) return;
    g_current.uptimeMs = millis();
    g_current.minHeap = ESP.getMinFreeHeap();
    g_current.clean = 1;
    flush(true);
    log_i("clean shutdown: %s", why ? why : "restart");
}

uint32_t BootLog::bootCount() { return g_current.index; }

uint8_t BootLog::crashStreak() { return g_crashStreak; }

String BootLog::asJson() {
    String out = "{\"boot\":";
    out += g_current.index;
    out += ",\"reason\":\"";
    out += reasonName(g_current.reason);
    out += "\",\"previous\":";
    if (g_hasPrevious) {
        appendRecord(out, g_previous);
    } else {
        out += "null";
    }
    out += ",\"history\":[";
    if (g_ready) {
        // Oldest first. The ring holds eight, so a device that reboots in a
        // loop shows the shape of the loop rather than only its last turn.
        const uint32_t total = g_current.index;
        const uint32_t start = total > kSlots ? total - kSlots : 1;
        bool first = true;
        for (uint32_t i = start; i <= total; ++i) {
            Record r{};
            if (g_prefs.getBytes(slotKey(static_cast<uint8_t>((i - 1) % kSlots)), &r,
                                 sizeof(r)) != sizeof(r)) {
                continue;
            }
            if (r.index != i) continue;
            if (!first) out += ',';
            appendRecord(out, r);
            first = false;
        }
    }
    out += "]}";
    return out;
}

}  // namespace appdiag
