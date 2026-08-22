#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"

namespace touchui {

constexpr int16_t kWidth = 40;
constexpr int16_t kX = appcfg::kScreenW - kWidth;
constexpr int16_t kItemHeight = 40;
constexpr uint8_t kItemCount = 6;
// Keep a 320 px composition centred in the 280 px viewport left of the rail.
constexpr int16_t kContentShift = kWidth / 2;

enum class Action : uint8_t {
    None = 0,
    Talk,
    Volume,
    Mute,
    Brightness,
    Sleep,
    Settings,
};

inline Action actionAt(int16_t x, int16_t y) {
    if (x < kX || x >= appcfg::kScreenW || y < 0 || y >= appcfg::kScreenH) {
        return Action::None;
    }
    return static_cast<Action>(1 + y / kItemHeight);
}

}  // namespace touchui
