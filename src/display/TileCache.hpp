#pragma once

#include <Arduino.h>

#include "AppTypes.hpp"

// A small LRU of decoded tiles held in SRAM.
//
// This is an optimisation, never a requirement: every miss simply streams the
// tile from SD instead. The cache earns its keep when the body animation is
// paused (sleep, low-power, sway disabled) and the same handful of eye and
// mouth tiles is drawn over and over.
class TileCache {
public:
    // Allocates up to budgetBytes, halving on failure down to minBytes.
    // Returns the number of bytes actually reserved.
    size_t begin(size_t entryBytes, size_t budgetBytes, size_t minBytes);
    void end();

    bool enabled() const { return slab_ != nullptr; }
    uint8_t slots() const { return slots_; }
    size_t entryBytes() const { return entryBytes_; }

    // Returns the cached payload for this key, or nullptr on a miss.
    const uint8_t* get(uint32_t key);
    // Reserves a slot for key and returns writable storage of entryBytes.
    // Call commit() once the payload is valid, or discard() if the fill failed.
    uint8_t* reserve(uint32_t key);
    void commit(uint8_t* slot);
    void discard(uint8_t* slot);

    void invalidateAll();

    uint32_t hits() const { return hits_; }
    uint32_t misses() const { return misses_; }

    static uint32_t makeKey(Expression e, uint8_t layer, uint16_t frame) {
        return (static_cast<uint32_t>(e) << 24) | (static_cast<uint32_t>(layer) << 20) | frame;
    }

private:
    struct Entry {
        uint32_t key = 0;
        uint32_t lastUse = 0;
        bool valid = false;
        bool pending = false;
    };

    uint8_t* slab_ = nullptr;
    Entry* entries_ = nullptr;
    uint8_t slots_ = 0;
    size_t entryBytes_ = 0;
    uint32_t useCounter_ = 0;
    uint32_t hits_ = 0;
    uint32_t misses_ = 0;
};
