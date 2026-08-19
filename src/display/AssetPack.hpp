#pragma once

#include <Arduino.h>
#include <FS.h>

#include "AppConfig.hpp"
#include "AppTypes.hpp"
#include "AssetFormat.hpp"

// Reads a character pack from the microSD card.
//
// A pack is a manifest plus a set of .m5a clips holding raw panel-native
// RGB565 frames. Nothing is decoded at runtime; a read is a seek plus a
// sequential byte copy, which is what lets a plain ESP32 animate at 30 Hz.
class AssetPack {
public:
    enum class Layer : uint8_t {
        Base = 0,   // full screen, one frame
        Sway,       // head/body region, swayFrames frames
        Eyes,       // eye region, swayFrames * kEyeSlots frames
        Mouth,      // mouth region, swayFrames * visemeFrames frames
        Count,
    };
    static constexpr uint8_t kLayerCount = static_cast<uint8_t>(Layer::Count);

    // rootDir is the pack directory, e.g. "/companion/packs/m5girl".
    bool load(const char* rootDir);
    void unload();
    bool ready() const { return ready_; }

    const char* name() const { return name_.c_str(); }
    const char* root() const { return root_.c_str(); }
    const char* error() const { return error_.c_str(); }

    Rect layerRect(Layer l) const { return rect_[static_cast<uint8_t>(l)]; }
    uint8_t swayFrames() const { return swayFrames_; }
    uint8_t visemeFrames() const { return visemeFrames_; }
    uint8_t format() const { return format_; }
    // Packs ship their own backdrop, so the status bar has to follow it.
    bool lightTheme() const { return lightTheme_; }

    // Total bytes one full tile of this layer occupies.
    size_t layerBytes(Layer l) const { return rect_[static_cast<uint8_t>(l)].bytes565(); }

    // Frame index helpers - keep the packer and the firmware in agreement.
    uint16_t swayIndex(uint8_t sway) const;
    uint16_t eyeIndex(uint8_t sway, uint8_t slot) const;
    uint16_t mouthIndex(uint8_t sway, uint8_t viseme) const;

    // Reads `rows` scan lines of a frame into dst. dst must hold
    // rect.w * rows * 2 bytes. Returns false on any I/O or bounds problem.
    bool readBand(Expression e, Layer l, uint16_t frame, uint16_t rowStart, uint16_t rows,
                  uint8_t* dst);

    // Reads a whole frame. Used to fill the RAM tile cache.
    bool readFrame(Expression e, Layer l, uint16_t frame, uint8_t* dst, size_t capacity);

    // Optional full-screen clips such as the boot animation.
    bool hasClip(const char* name) const;
    bool readClipBand(const char* name, uint16_t frame, uint16_t rowStart, uint16_t rows,
                      uint8_t* dst, uint16_t& outWidth);
    uint16_t clipFrameCount(const char* name);
    // Path of the largest clip, used for the boot-time throughput benchmark.
    const char* benchmarkPath() const { return benchPath_.c_str(); }

private:
    struct OpenFile {
        String path;
        File file;
        m5a::Header header{};
        uint32_t lastUse = 0;
        bool valid = false;
    };

    OpenFile* acquire(const String& path);
    const String& clipPath(Expression e, Layer l) const;

    bool ready_ = false;
    String root_;
    String name_;
    String error_;
    uint8_t format_ = m5a::kFormatRgb565Be;
    bool lightTheme_ = false;
    uint8_t swayFrames_ = 1;
    uint8_t visemeFrames_ = 1;
    Rect rect_[kLayerCount];
    String path_[kExpressionCount][kLayerCount];
    String bootClip_;
    String benchPath_;
    OpenFile open_[appcfg::kOpenFileCacheSlots];
    uint32_t useCounter_ = 0;
};
