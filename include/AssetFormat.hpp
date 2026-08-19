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
    // Frames are stored as the tiles that changed since the previous frame,
    // rather than in full. See "Tile-delta clips" below.
    kFlagTileDelta = 1 << 1,
};

// Side of a square tile in a tile-delta clip, in pixels. 16 keeps the index
// small (300 entries for a whole screen) while still being fine enough that a
// head moving a few pixels does not dirty the shoulders.
constexpr uint16_t kTileSide = 16;
constexpr uint32_t kTileBytes = kTileSide * kTileSide * 2;
// A full 320x240 screen is 20x15 tiles.
constexpr uint16_t kMaxTilesPerFrame = 320;

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
// Tile-delta clips (kFlagTileDelta)
//
// A full-screen frame is 150 KB, and the LCD and the SD card share one SPI bus
// carrying about 850 KB/s in total - so a clip stored as whole frames plays at
// 5.5 fps however short it is. Measured on the Kizuna pack, only 59% of a
// frame differs from the one before it, and 36% of that is the body moving
// when it did not need to. Sending just the tiles that changed is what takes a
// gesture from a slideshow to something that reads as motion.
//
// Layout when the flag is set:
//
//   [ 32 byte header ]
//   [ uint32 frameOffset[frameCount + 1] ]   byte offsets from the file start
//   [ frame 0 ][ frame 1 ] ... [ frame N-1 ]
//
// frameOffset[i+1] - frameOffset[i] is frame i's size, and the extra last
// entry is the end of the data, so no frame needs the next one's header to be
// read. Each frame is:
//
//   [ uint16 tileCount ]
//   [ uint16 tileIndex[tileCount] ]   tile number, row major: ty * tilesX + tx
//   [ tileCount * kTileBytes of pixels, in index order ]
//
// Frame 0 always carries every tile, so playback can start there without a
// prior frame. `frameBytes` in the header is the largest frame payload rather
// than a fixed stride; nothing seeks by multiplying it.
constexpr uint32_t kFrameTableBytes(uint16_t frameCount) {
    return static_cast<uint32_t>(frameCount + 1) * sizeof(uint32_t);
}

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
