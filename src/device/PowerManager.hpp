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

    // Whether the IP5306 accepted the bit that stops it switching itself off
    // under a light load. On a unit whose PMIC is not wired to I2C it never
    // will, and that is worth knowing before blaming the firmware for a device
    // that vanishes when the cable comes out.
    bool boostHeld() const { return boostHeld_; }

    // Forgets what the panel was last set to. For a caller that has written
    // the backlight directly - a load test, say - and needs the next update to
    // put it back rather than skip it as already correct.
    void invalidate() { applied_ = -1; }

private:
    void applyBrightness(uint8_t value);
    static bool keepBoostOn();

    uint8_t percent_ = 0;
    bool charging_ = false;
    uint8_t active_ = 160;
    int16_t applied_ = -1;   // -1 = unknown, so the next update writes it
    bool boostHeld_ = false;
    int32_t smoothed_ = -1;
    uint32_t lastSampleMs_ = 0;
};
