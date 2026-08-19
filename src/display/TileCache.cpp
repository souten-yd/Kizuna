#include "TileCache.hpp"

#include <new>

size_t TileCache::begin(size_t entryBytes, size_t budgetBytes, size_t minBytes) {
    end();
    if (!entryBytes) return 0;
    entryBytes_ = entryBytes;

    size_t budget = budgetBytes;
    while (budget >= minBytes && budget >= entryBytes) {
        const uint8_t slots = static_cast<uint8_t>(budget / entryBytes);
        // The cache is allocated during boot, before Wi-Fi has been brought
        // up, so the heap looks far roomier here than it will be a second
        // later. Reserve enough for the whole network stack plus working
        // room - starving Wi-Fi to cache pixels is a bad trade on a 520 KiB
        // part with no PSRAM.
        if (ESP.getFreeHeap() > slots * entryBytes + 110 * 1024) {
            slab_ = static_cast<uint8_t*>(malloc(slots * entryBytes));
            if (slab_) {
                entries_ = new (std::nothrow) Entry[slots];
                if (!entries_) {
                    free(slab_);
                    slab_ = nullptr;
                } else {
                    slots_ = slots;
                    log_i("tile cache: %u slots x %u B = %u B", slots, entryBytes,
                          slots * entryBytes);
                    return slots_ * entryBytes_;
                }
            }
        }
        budget /= 2;
    }
    log_w("tile cache disabled (heap %u)", ESP.getFreeHeap());
    return 0;
}

void TileCache::end() {
    if (slab_) free(slab_);
    delete[] entries_;
    slab_ = nullptr;
    entries_ = nullptr;
    slots_ = 0;
}

const uint8_t* TileCache::get(uint32_t key) {
    if (!slab_) return nullptr;
    for (uint8_t i = 0; i < slots_; ++i) {
        if (entries_[i].valid && entries_[i].key == key) {
            entries_[i].lastUse = ++useCounter_;
            ++hits_;
            return slab_ + static_cast<size_t>(i) * entryBytes_;
        }
    }
    ++misses_;
    return nullptr;
}

uint8_t* TileCache::reserve(uint32_t key) {
    if (!slab_) return nullptr;
    uint8_t victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (uint8_t i = 0; i < slots_; ++i) {
        if (entries_[i].pending) continue;
        if (!entries_[i].valid) {
            victim = i;
            oldest = 0;
            break;
        }
        if (entries_[i].lastUse < oldest) {
            oldest = entries_[i].lastUse;
            victim = i;
        }
    }
    if (oldest == UINT32_MAX) return nullptr;  // everything is pending

    entries_[victim].key = key;
    entries_[victim].valid = false;
    entries_[victim].pending = true;
    return slab_ + static_cast<size_t>(victim) * entryBytes_;
}

void TileCache::commit(uint8_t* slot) {
    if (!slab_ || !slot) return;
    const size_t idx = static_cast<size_t>(slot - slab_) / entryBytes_;
    if (idx >= slots_) return;
    entries_[idx].valid = true;
    entries_[idx].pending = false;
    entries_[idx].lastUse = ++useCounter_;
}

void TileCache::discard(uint8_t* slot) {
    if (!slab_ || !slot) return;
    const size_t idx = static_cast<size_t>(slot - slab_) / entryBytes_;
    if (idx >= slots_) return;
    entries_[idx].valid = false;
    entries_[idx].pending = false;
}

void TileCache::invalidateAll() {
    for (uint8_t i = 0; i < slots_; ++i) {
        entries_[i].valid = false;
        entries_[i].pending = false;
    }
}
