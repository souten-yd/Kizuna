#pragma once

#include <Adafruit_NeoPixel.h>

#include "AppTypes.hpp"

// The 10 SK6812 pixels along the M5GO sides.
//
// They exist so the companion's state is readable from across the room, with
// the screen facing away. Every pattern is therefore about state, never
// decoration.
class LedController {
public:
    LedController();
    void begin(uint8_t brightness);
    void setBrightness(uint8_t brightness);
    // What is actually set, which is not always what the config says: on
    // battery the bar is switched off without the stored setting changing.
    // A caller that borrows the strip has to put it back here, not there.
    uint8_t brightness() const { return brightness_; }
    void update(CompanionState state, uint8_t audioLevel, bool serverOnline, bool muted,
                uint32_t nowMs);
    void off();

private:
    void fill(uint32_t color);
    void meter(uint8_t level, uint32_t color);

    Adafruit_NeoPixel pixels_;
    uint8_t brightness_ = 40;
    bool darkened_ = false;
    uint32_t lastUpdateMs_ = 0;
};
