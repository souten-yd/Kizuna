#include "SdCard.hpp"

#include <SD.h>
#include <SPI.h>

#include "AppConfig.hpp"

namespace {
// Descending retry ladder. A long or noisy TF connector often works at 10 MHz
// when it refuses to enumerate at 20 MHz.
constexpr uint32_t kFreqLadder[] = {appcfg::kSdFreqHz, 10000000, 4000000};
}  // namespace

bool SdCard::tryMount(uint32_t freqHz) {
    if (SD.begin(appcfg::kSdCsPin, SPI, freqHz, "/sd", appcfg::kSdMaxOpenFiles)) {
        mountFreq_ = freqHz;
        return true;
    }
    SD.end();
    return false;
}

bool SdCard::mount() {
    if (status_ == Status::Mounted) return true;

    for (uint32_t freq : kFreqLadder) {
        if (tryMount(freq)) {
            status_ = Status::Mounted;
            cardSize_ = SD.cardSize();
            log_i("SD mounted @%u Hz, %llu MB", freq, cardSize_ / (1024ULL * 1024ULL));
            return true;
        }
        delay(30);
    }

    // Distinguish "no card" from "card present but FATFS said no". The card
    // struct is initialised by SD.begin() before the filesystem is mounted, so
    // a valid card type after a failed begin() means the partition could not be
    // read - on a 64 GB card that is almost always exFAT.
    status_ = (SD.cardType() == CARD_NONE) ? Status::NoCard : Status::UnreadableFilesystem;
    log_w("SD mount failed: %s", statusText());
    return false;
}

void SdCard::unmount() {
    if (status_ == Status::Mounted) SD.end();
    status_ = Status::NotMounted;
    cardSize_ = 0;
    readBps_ = 0;
}

const char* SdCard::statusText() const {
    switch (status_) {
        case Status::Mounted: return "mounted";
        case Status::NoCard: return "no card";
        case Status::UnreadableFilesystem: return "unreadable filesystem";
        default: return "not mounted";
    }
}

const char* SdCard::hint() const {
    switch (status_) {
        case Status::UnreadableFilesystem:
            return "Reformat the card as FAT32 (32 KiB clusters). exFAT is not supported.";
        case Status::NoCard:
            return "Insert a FAT32 microSD card and reboot.";
        default:
            return "";
    }
}

uint32_t SdCard::measure(const char* path, size_t maxBytes) {
    if (!mounted() || !path) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;

    // Reuse the display band size so the measurement reflects the access
    // pattern the renderer actually uses.
    static uint8_t buf[appcfg::kBandBytes];
    const uint32_t start = micros();
    size_t total = 0;
    while (total < maxBytes) {
        const int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    const uint32_t elapsed = micros() - start;
    f.close();

    if (!elapsed || total < sizeof(buf)) return 0;
    readBps_ = static_cast<uint32_t>((static_cast<uint64_t>(total) * 1000000ULL) / elapsed);
    log_i("SD read benchmark: %u B in %u us -> %u B/s", total, elapsed, readBps_);
    return readBps_;
}
