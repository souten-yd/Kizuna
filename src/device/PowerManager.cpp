#include "PowerManager.hpp"

#include <M5Unified.h>

#include "Board.hpp"

namespace {
constexpr uint32_t kSampleIntervalMs = 5000;
// Off, not dim. A companion sits on a desk showing the same face for hours, and
// a backlight held at a low level still ages the panel and still lights the
// room at night. The right button held down toggles it; nothing else turns it
// off, so the state is always something the user chose. Asleep the eyes stay
// shut and the frame stops changing, so the renderer has nothing to send either
// - the panel goes dark and the bus goes quiet together.
constexpr uint8_t kDimBrightness = 0;
constexpr uint8_t kBrightnessSteps[] = {60, 110, 160, 220};
}  // namespace

bool PowerManager::keepBoostOn() {
    // The IP5306 on the classic Core switches its boost converter off when it
    // decides the load is too small to be worth supplying - a power bank's
    // instinct, in a thing that is not a power bank. Pull the USB and the load
    // drops; turn the screen off as well and it drops further; the chip stops
    // supplying and the device is simply gone, which is why every restart
    // reports "poweron" rather than a crash. It is not switching to battery
    // badly, it is being switched off.
    //
    // The M5Stack forum's answer to this is a hardware modification - an ideal
    // diode across the supply, or a diode across the chip's own pins - because
    // there is no clean way to hold the rail up from software. But the chip
    // does have a bit for exactly this, reachable over I2C on the units whose
    // IP5306 is wired to it, and setting it is free.
#if M5COMPANION_BOARD_CORES3
    // The CoreS3's AXP2101 has no such habit and no such register; the symbol
    // does not exist in M5Unified for this target either.
    return false;
#else
    if (M5.Power.getType() != m5::Power_Class::pmic_t::pmic_ip5306) return false;
    return M5.Power.Ip5306.setPowerBoostKeepOn(true);
#endif
}

void PowerManager::begin(uint8_t activeBrightness) {
    active_ = activeBrightness;
    applyBrightness(active_);
    boostHeld_ = keepBoostOn();
    if constexpr (board::kNeedsIp5306BoostHold) {
        log_i("IP5306 boost keep-on: %s", boostHeld_ ? "set" : "unavailable");
    }
    percent_ = static_cast<uint8_t>(constrain(M5.Power.getBatteryLevel(), 0, 100));
    smoothed_ = percent_ * 16;
}

void PowerManager::applyBrightness(uint8_t value) {
    if (applied_ == static_cast<int16_t>(value)) return;
    applied_ = value;
    M5.Display.setBrightness(value);
}

void PowerManager::setActiveBrightness(uint8_t value) {
    active_ = value;
    applyBrightness(active_);
}

uint8_t PowerManager::cycleBrightness() {
    uint8_t next = kBrightnessSteps[0];
    for (size_t i = 0; i < sizeof(kBrightnessSteps); ++i) {
        if (kBrightnessSteps[i] > active_) {
            next = kBrightnessSteps[i];
            break;
        }
    }
    setActiveBrightness(next);
    return next;
}

void PowerManager::update(uint32_t nowMs, bool dimmed) {
    applyBrightness(dimmed ? kDimBrightness : active_);

    if (nowMs - lastSampleMs_ < kSampleIntervalMs && smoothed_ >= 0) return;
    lastSampleMs_ = nowMs;

    const int raw = constrain(M5.Power.getBatteryLevel(), 0, 100);
    smoothed_ = smoothed_ < 0 ? raw * 16 : smoothed_ + (raw * 16 - smoothed_) / 4;
    percent_ = static_cast<uint8_t>(smoothed_ / 16);
    charging_ = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
}
