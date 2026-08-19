#include "LedController.hpp"

#include "AppConfig.hpp"

namespace {
constexpr uint32_t kUpdateIntervalMs = 40;

uint8_t breathe(uint32_t nowMs, uint32_t periodMs, uint8_t lo, uint8_t hi) {
    const uint32_t phase = nowMs % periodMs;
    const uint32_t half = periodMs / 2;
    const uint32_t up = phase < half ? phase : periodMs - phase;
    return static_cast<uint8_t>(lo + (hi - lo) * up / half);
}
}  // namespace

LedController::LedController()
    : pixels_(appcfg::kLedCount, appcfg::kLedPin, NEO_GRB + NEO_KHZ800) {}

void LedController::begin(uint8_t brightness) {
    pixels_.begin();
    brightness_ = brightness;
    pixels_.setBrightness(brightness_);
    off();
}

void LedController::setBrightness(uint8_t brightness) {
    brightness_ = brightness;
    pixels_.setBrightness(brightness_);
}

void LedController::off() {
    pixels_.clear();
    pixels_.show();
}

void LedController::fill(uint32_t color) {
    for (uint8_t i = 0; i < appcfg::kLedCount; ++i) pixels_.setPixelColor(i, color);
}

void LedController::meter(uint8_t level, uint32_t color) {
    const uint8_t lit = min<uint8_t>(appcfg::kLedCount, 2 + level * 2);
    for (uint8_t i = 0; i < lit; ++i) pixels_.setPixelColor(i, color);
}

void LedController::update(CompanionState state, uint8_t audioLevel, bool serverOnline, bool muted,
                           uint32_t nowMs) {
    if (nowMs - lastUpdateMs_ < kUpdateIntervalMs) return;
    lastUpdateMs_ = nowMs;

    pixels_.clear();
    pixels_.setBrightness(brightness_);

    switch (state) {
        case CompanionState::Sleep:
            pixels_.show();
            return;

        case CompanionState::Listening:
            if (muted) {
                // Muted but listening would be a lie; show a red floor instead.
                fill(pixels_.Color(40, 0, 0));
            } else {
                meter(audioLevel, pixels_.Color(0, 60, 150));
            }
            break;

        case CompanionState::Thinking: {
            // A comet running along the strip reads as "working" much better
            // than a static colour.
            const uint8_t head = (nowMs / 90) % appcfg::kLedCount;
            for (uint8_t t = 0; t < 4; ++t) {
                const uint8_t i = (head + appcfg::kLedCount - t) % appcfg::kLedCount;
                const uint8_t f = 255 >> (t + 1);
                pixels_.setPixelColor(i, pixels_.Color(f, f * 2 / 5, 0));
            }
            break;
        }

        case CompanionState::Speaking:
            meter(audioLevel, pixels_.Color(255, 90, 0));
            break;

        case CompanionState::Error:
            if ((nowMs / 320) % 2) fill(pixels_.Color(180, 0, 0));
            break;

        case CompanionState::Boot: {
            const uint8_t head = (nowMs / 60) % appcfg::kLedCount;
            pixels_.setPixelColor(head, pixels_.Color(0, 120, 200));
            break;
        }

        default: {
            const uint8_t b = breathe(nowMs, 4200, 6, 34);
            const uint32_t c =
                serverOnline ? pixels_.Color(0, b, b * 2) : pixels_.Color(b * 2, b / 2, 0);
            fill(c);
            break;
        }
    }

    pixels_.show();
}
