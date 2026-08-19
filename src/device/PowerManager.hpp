#pragma once

#include <Arduino.h>

// Battery reporting and screen dimming.
//
// Sampling the fuel gauge is cheap but noisy, so the percentage is smoothed;
// a companion whose battery readout jitters between 71 and 68 percent looks
// broken even when it is not.
class PowerManager {
public:
    void begin(uint8_t activeBrightness);
    void update(uint32_t nowMs, bool dimmed);

    uint8_t batteryPercent() const { return percent_; }
    bool charging() const { return charging_; }
    void setActiveBrightness(uint8_t value);
    uint8_t activeBrightness() const { return active_; }
    uint8_t cycleBrightness();

private:
    void applyBrightness(uint8_t value);

    uint8_t percent_ = 0;
    bool charging_ = false;
    uint8_t active_ = 160;
    uint8_t applied_ = 0;
    int32_t smoothed_ = -1;
    uint32_t lastSampleMs_ = 0;
};
