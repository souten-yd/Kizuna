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
    void update(CompanionState state, uint8_t audioLevel, bool serverOnline, bool muted,
                uint32_t nowMs);
    void off();

private:
    void fill(uint32_t color);
    void meter(uint8_t level, uint32_t color);

    Adafruit_NeoPixel pixels_;
    uint8_t brightness_ = 40;
    uint32_t lastUpdateMs_ = 0;
};
