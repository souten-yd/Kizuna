#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"

namespace touchui {

// Four generous targets fill the whole right edge. 52 x 60 px is large enough
// to hit with the pad of a thumb without taking too much room from the face.
constexpr int16_t kWidth = 52;
constexpr int16_t kX = appcfg::kScreenW - kWidth;
constexpr int16_t kItemHeight = appcfg::kScreenH / 4;
constexpr uint8_t kItemCount = 4;
constexpr uint32_t kLongPressMs = 650;
// Keep a 320 px composition centred in the viewport left of the rail.
constexpr int16_t kContentShift = kWidth / 2;

enum class Action : uint8_t {
    None = 0,
    Talk,
    Volume,
    Brightness,
    Settings,
};

inline Action actionAt(int16_t x, int16_t y) {
    if (x < kX || x >= appcfg::kScreenW || y < 0 || y >= appcfg::kScreenH) {
        return Action::None;
    }
    return static_cast<Action>(1 + y / kItemHeight);
}

}  // namespace touchui
