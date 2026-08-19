#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"

// Decides how many bytes of artwork may move across the shared SPI bus in one
// display tick.
//
// On M5GO the LCD and the TF card sit on the same MOSI/MISO/SCK lines, so a
// tile costs its own size twice: once reading it off the card, once writing it
// to the panel. Those two costs are what this class adds up. The SD half is
// measured at boot instead of assumed, because a UHS-I card and a tired old
// class-4 card differ by more than 3x and the animation should degrade
// gracefully rather than stutter.
class FrameBudget {
public:
    void configure(uint32_t sdBytesPerSec) {
        sdBps_ = sdBytesPerSec ? sdBytesPerSec : appcfg::kAssumedSdBytesPerSec;
        // Seconds per byte for the read + write round trip, in nanoseconds to
        // stay in integer maths.
        const uint32_t nsPerByte =
            1000000000UL / sdBps_ + 1000000000UL / appcfg::kLcdBytesPerSec;
        bytesPerTick_ = static_cast<uint32_t>((appcfg::kMaxTickBusMs * 1000000ULL) / nsPerByte);
        if (bytesPerTick_ < 4096) bytesPerTick_ = 4096;
    }

    void beginTick() { remaining_ = bytesPerTick_; }

    bool canAfford(size_t bytes) const { return bytes <= remaining_; }
    // Always lets the first request of a tick through, so a layer larger than
    // one whole tick still makes progress instead of starving forever.
    bool take(size_t bytes) {
        if (bytes > remaining_) {
            if (remaining_ == bytesPerTick_) {
                remaining_ = 0;
                return true;
            }
            return false;
        }
        remaining_ -= bytes;
        return true;
    }

    uint32_t bytesPerTick() const { return bytesPerTick_; }
    uint32_t sdBytesPerSec() const { return sdBps_; }
    uint32_t estimatedBytesPerSec() const { return bytesPerTick_ * appcfg::kDisplayTickHz; }

private:
    uint32_t sdBps_ = appcfg::kAssumedSdBytesPerSec;
    uint32_t bytesPerTick_ = 16384;
    uint32_t remaining_ = 0;
};
