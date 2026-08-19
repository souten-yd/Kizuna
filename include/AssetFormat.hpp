// .m5a - M5Companion Animation container
//
// Design goal: the ESP32 must never decode anything at runtime. Frames are
// stored as raw, panel-native RGB565 so a frame band goes straight from the
// SD card into the LCD DMA path. Cheap on CPU, expensive on bytes - which is
// exactly the trade a 64 GB card is for.
//
// Layout:
//   [ 32 byte header ][ frame 0 ][ frame 1 ] ... [ frame N-1 ]
//
// Every frame is frameBytes = width * height * 2 and frame i starts at
// kHeaderBytes + i * frameBytes, so seeking is a single arithmetic step.
#pragma once

#include <stdint.h>

namespace m5a {

constexpr uint32_t kMagic = 0x3141354D;  // 'M','5','A','1' little-endian
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 32;

enum PixelFormat : uint8_t {
    // RGB565, big-endian (high byte first). This is the ILI9341 wire order,
    // so M5GFX can push it with zero per-pixel conversion.
    kFormatRgb565Be = 0,
    // RGB565 in ESP32 native little-endian order. Needs a byte swap on push.
    kFormatRgb565Le = 1,
};

enum Flags : uint16_t {
    kFlagNone = 0,
    // Frames form a ping-pong loop (0,1,..,N-1,N-2,..,1) instead of wrapping.
    kFlagPingPong = 1 << 0,
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;        // kMagic
    uint16_t version;      // kVersion
    uint16_t flags;        // Flags
    uint16_t width;
    uint16_t height;
    uint16_t frameCount;
    uint16_t fps;          // playback hint, 0 = driven by the application
    uint32_t frameBytes;   // width * height * 2
    uint8_t  format;       // PixelFormat
    uint8_t  reserved0[3];
    uint32_t reserved1;
    uint32_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderBytes, "m5a header must be 32 bytes");

// ---------------------------------------------------------------------------
// Frame indexing conventions
//
// The packer bakes the idle body sway into every layer, so a tile always
// matches the body pose underneath it. Tile rectangles never move on screen -
// only their contents do - which keeps the renderer free of offset bookkeeping.
//
//   sway  clip : frame = sway
//   eyes  clip : frame = sway * kEyeSlots + slot
//   mouth clip : frame = sway * visemeCount + viseme
//   base  clip : frame = 0 (sway pose 0, full screen)
// ---------------------------------------------------------------------------
// The eye slots the packer emits, in this order. Each one maps to a real
// drawn cell on the artist's sheets rather than to a warped copy of the open
// eye, which is why a blink here reads as animation instead of as a glitch.
enum EyeSlot : uint8_t {
    kEyeOpenCenter = 0,
    kEyeOpenLeft,
    kEyeOpenRight,
    kEyeOpenUp,
    kEyeOpenDown,
    kEyeSoftLower,      // barely lowered lids - the resting idle pose
    kEyeHalf,
    kEyeAlmostClosed,
    kEyeClosed,
    kEyeWide,           // attentive / startled
    kEyeSleepyHalf,
    kEyeSleepyClosed,
    kEyeSlots,
};

}  // namespace m5a
