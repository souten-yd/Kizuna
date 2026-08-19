#include "PowerManager.hpp"

#include <M5Unified.h>

namespace {
constexpr uint32_t kSampleIntervalMs = 5000;
constexpr uint8_t kDimBrightness = 12;
constexpr uint8_t kBrightnessSteps[] = {60, 110, 160, 220};
}  // namespace

void PowerManager::begin(uint8_t activeBrightness) {
    active_ = activeBrightness;
    applyBrightness(active_);
    percent_ = static_cast<uint8_t>(constrain(M5.Power.getBatteryLevel(), 0, 100));
    smoothed_ = percent_ * 16;
}

void PowerManager::applyBrightness(uint8_t value) {
    if (value == applied_) return;
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
