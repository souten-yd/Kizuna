#pragma once

#include <Arduino.h>

// Owns the microSD card on the shared VSPI bus.
//
// Important: on M5GO the LCD and the TF slot share MOSI/MISO/SCK. Every call
// here must therefore happen on the display task, which is the single owner of
// that bus. See docs/ARCHITECTURE.md.
class SdCard {
public:
    enum class Status : uint8_t {
        NotMounted,
        Mounted,
        NoCard,
        UnreadableFilesystem,  // card responds but FATFS refused it (exFAT?)
    };

    bool mount();
    void unmount();

    Status status() const { return status_; }
    bool mounted() const { return status_ == Status::Mounted; }

    uint64_t cardSizeBytes() const { return cardSize_; }
    uint32_t mountFreqHz() const { return mountFreq_; }
    const char* statusText() const;
    const char* hint() const;

    // Sequential read throughput in bytes/sec, measured on a real asset file.
    // Returns 0 until measure() has run.
    uint32_t readBytesPerSec() const { return readBps_; }
    uint32_t measure(const char* path, size_t maxBytes = 512 * 1024);

private:
    bool tryMount(uint32_t freqHz);

    Status status_ = Status::NotMounted;
    uint64_t cardSize_ = 0;
    uint32_t mountFreq_ = 0;
    uint32_t readBps_ = 0;
};
