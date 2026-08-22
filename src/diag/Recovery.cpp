#include "Recovery.hpp"

#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include "ResetReason.hpp"

// Take the rollback decision back from the Arduino core.
//
// The bootloader that ships with arduino-esp32 is built with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so a freshly installed image boots
// once in ESP_OTA_IMG_PENDING_VERIFY and is reverted at the next restart
// unless something marks it valid. The core's initArduino() marks it valid
// immediately - before setup() has run, before the display task exists, before
// anything the update might have broken has had a chance to break. That makes
// the mechanism nearly worthless: only an image that fails inside Espressif's
// own startup code would ever be rolled back.
//
// Returning true here defers the decision to us. It must be extern "C": the
// weak symbol it overrides is declared in a C file, and a C++ definition
// silently fails to replace it (espressif/arduino-esp32#7423).
extern "C" bool verifyRollbackLater() { return true; }

namespace appdiag {
namespace {

constexpr uint32_t kProbationMs = 60000;
// Three consecutive boots that never got that far. Two would fire on a pair of
// unlucky power cuts; four leaves someone watching a device reboot for a long
// time before it admits there is a problem.
constexpr uint8_t kSafeModeStreak = 3;
// The flash-backed count needs one more, because it can only be read after the
// fact: by the time three crashes are on record the third has already
// happened, whereas the RTC counter sees the boot that is starting now.
constexpr uint8_t kSafeModeCrashStreak = 3;

constexpr uint32_t kStreakMagic = 0x4d355242;  // 'M5RB'

// RTC memory: kept across a panic, a watchdog and a software reset, cleared by
// a power cut. That last part is deliberate. Pulling the power is how a person
// says "start again", and it should mean that.
RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR uint32_t g_streak;
RTC_NOINIT_ATTR uint32_t g_tried;   // bit 0: the other slot, bit 1: factory

constexpr uint32_t kTriedOther = 1u << 0;
constexpr uint32_t kTriedFactory = 1u << 1;

const esp_partition_t* otherOtaSlot() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return nullptr;
    const esp_partition_subtype_t want = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0
                                             ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                             : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, want, nullptr);
}

const char* label(const esp_partition_t* p) {
    if (!p) return "none";
    switch (p->subtype) {
        case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
        case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return "app0";
        case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return "app1";
        default:                                return "app?";
    }
}

bool holdsImage(const esp_partition_t* p) {
    // The first byte of an ESP32 application image is 0xE9. This is the same
    // cheap check the Update library makes before it will boot a partition,
    // and it is not a promise the image works - nothing short of booting it
    // is - only that there is something there rather than erased flash.
    if (!p) return false;
    uint8_t magic = 0;
    return esp_partition_read(p, 0, &magic, 1) == ESP_OK && magic == 0xE9;
}

}  // namespace

bool Recovery::safeMode_ = false;
bool Recovery::confirmed_ = false;
bool Recovery::proven_ = false;
uint8_t Recovery::streak_ = 0;
uint32_t Recovery::healthySinceMs_ = 0;

void Recovery::begin(uint8_t crashStreak) {
    if (g_magic != kStreakMagic) {
        g_magic = kStreakMagic;
        g_streak = 0;
        g_tried = 0;
    }
    g_streak = g_streak < 250 ? g_streak + 1 : 250;
    streak_ = static_cast<uint8_t>(g_streak);

    safeMode_ = streak_ >= kSafeModeStreak || crashStreak >= kSafeModeCrashStreak;
    confirmed_ = !onTrial();

    log_i("recovery: running %s, boot streak %u, crash streak %u%s%s", runningLabel(),
          (unsigned)streak_, (unsigned)crashStreak,
          onTrial() ? ", ON TRIAL" : "", safeMode_ ? ", SAFE MODE" : "");
    if (onTrial()) {
        log_w("this image is on probation - it reverts to %s unless it stays up %u s",
              otherLabel(), (unsigned)(kProbationMs / 1000));
    }
    if (safeMode_) {
        log_e("boot streak %u, crash streak %u - starting without the pack or audio",
              (unsigned)streak_, (unsigned)crashStreak);
    }
}

bool Recovery::onTrial() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

void Recovery::update(uint32_t nowMs, bool healthy) {
    if (!healthy) {
        healthySinceMs_ = 0;
        return;
    }
    if (!healthySinceMs_) healthySinceMs_ = nowMs ? nowMs : 1;
    if (nowMs - healthySinceMs_ < kProbationMs) return;

    if (!proven_) {
        proven_ = true;
        log_i("recovery: %u s healthy - the image has earned a look at the rest",
              (unsigned)(kProbationMs / 1000));
    }
}

void Recovery::confirm() {
    if (confirmed_ && !g_streak) return;

    if (!confirmed_) {
        confirmed_ = true;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            log_i("recovery: image confirmed; rollback cancelled");
        }
    }
    // Safe mode holds the streak deliberately. Coming up reduced and working
    // is not evidence that the full firmware works, and clearing the count
    // here would put the device straight back into the loop it just escaped.
    if (!safeMode_ && g_streak) {
        g_streak = 0;
        g_tried = 0;
        streak_ = 0;
    }
}

bool Recovery::triedOtherSlot() { return (g_tried & kTriedOther) != 0; }
void Recovery::noteTriedOtherSlot() { g_tried |= kTriedOther; }
bool Recovery::triedFactory() { return (g_tried & kTriedFactory) != 0; }
void Recovery::noteTriedFactory() { g_tried |= kTriedFactory; }

void Recovery::clearBootLoop() {
    g_magic = kStreakMagic;
    g_streak = 0;
    g_tried = 0;
    streak_ = 0;
    log_i("recovery: boot loop counter cleared by request");
}

namespace {
const esp_partition_t* factorySlot() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}
}  // namespace

bool Recovery::factoryPresent() { return holdsImage(factorySlot()); }

bool Recovery::selectFactory() {
    const esp_partition_t* factory = factorySlot();
    if (!holdsImage(factory)) {
        log_e("recovery: no recovery application is installed");
        return false;
    }
    if (esp_ota_set_boot_partition(factory) != ESP_OK) {
        log_e("recovery: could not select the factory partition");
        return false;
    }
    return true;
}

bool Recovery::bootFactory() {
    if (!selectFactory()) return false;
    log_w("recovery: starting the recovery application");
    delay(200);
    ESP.restart();
    return true;
}

const char* Recovery::runningLabel() { return label(esp_ota_get_running_partition()); }
const char* Recovery::otherLabel() { return label(otherOtaSlot()); }
bool Recovery::otherSlotBootable() { return holdsImage(otherOtaSlot()); }

bool Recovery::bootOtherSlot() {
    const esp_partition_t* other = otherOtaSlot();
    if (!holdsImage(other)) {
        log_e("recovery: %s holds no image; staying here", label(other));
        return false;
    }
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        log_e("recovery: could not select %s", label(other));
        return false;
    }
    log_w("recovery: switching to %s and restarting", label(other));
    delay(200);
    ESP.restart();
    return true;
}

void Recovery::appendJson(String& out) {
    out += "{\"running\":\"";
    out += runningLabel();
    out += "\",\"other\":\"";
    out += otherLabel();
    out += "\",\"other_bootable\":";
    out += otherSlotBootable() ? "true" : "false";
    out += ",\"on_trial\":";
    out += onTrial() ? "true" : "false";
    out += ",\"confirmed\":";
    out += confirmed_ ? "true" : "false";
    out += ",\"proven\":";
    out += proven_ ? "true" : "false";
    out += ",\"safe_mode\":";
    out += safeMode_ ? "true" : "false";
    out += ",\"boot_streak\":";
    out += streak_;
    out += ",\"factory\":";
    out += factoryPresent() ? "true" : "false";
    out += '}';
}

}  // namespace appdiag
