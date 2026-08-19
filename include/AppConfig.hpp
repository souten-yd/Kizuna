// M5Companion - global compile-time configuration
//
// All tunables live here so board revisions and SD cards of different speed
// classes can be adapted without touching subsystem code.
#pragma once

#include <Arduino.h>

namespace appcfg {

// ---------------------------------------------------------------- system ---
constexpr uint32_t kSerialBaud = 115200;
constexpr const char* kNvsNamespace = "m5companion";

// --------------------------------------------------------------- display ---
constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;

// The display task wakes at this rate. Not every layer is redrawn on every
// tick; the frame budget decides what fits (see FrameBudget).
constexpr uint32_t kDisplayTickHz = 30;
constexpr uint32_t kDisplayTickMs = 1000 / kDisplayTickHz;

// Rows transferred per SD->LCD band. 16 rows x 320 px x 2 B = 10 KiB.
// Two bands are allocated so an SD read can overlap the previous DMA push.
constexpr uint16_t kBandRows = 16;
constexpr size_t kBandBytes = static_cast<size_t>(kScreenW) * kBandRows * 2;

// Conservative fallback used until the boot-time SD benchmark completes.
constexpr uint32_t kAssumedSdBytesPerSec = 1200000;
// Measured ILI9341 @40 MHz write throughput, minus protocol overhead.
constexpr uint32_t kLcdBytesPerSec = 4200000;
// Never let one tick monopolise the bus for longer than this.
constexpr uint32_t kMaxTickBusMs = 26;

// Full-screen gesture clips are intentionally slower than the 30 Hz tile loop:
// a 320x240 RGB565 frame is 150 KiB and SD/LCD share the same SPI bus.
// Tile-delta clips send about 90 KB per frame against the bus's 850 KB/s
// rather than a whole 150 KB screen, so the ceiling moved from 5 fps to
// roughly 9 - and to about 15 once the artwork stops moving the body
// under a head gesture. Frames that overrun simply take longer; this is
// the rate to aim at, not a promise.
constexpr uint16_t kGestureMaxFps = 15;

// ----------------------------------------------------------------- cache ---
// RAM budget for decoded tiles (eyes / mouth). Kept well under what the heap
// can spare at boot: the Wi-Fi stack has not started yet at that point and
// will want roughly 50 KiB of its own.
// heap cannot satisfy it; the renderer then falls back to streaming from SD.
constexpr size_t kTileCacheBytes = 48 * 1024;
constexpr size_t kTileCacheMinBytes = 12 * 1024;
// Heap the tile cache must leave behind. It is allocated during boot, before
// the network stack exists, so the heap looks far roomier there than it will
// be a second later - measured on this board, Wi-Fi plus the WebSocket client
// plus the pack web server cost about 56 KB. Reserving only the offline
// figure left 39 KB free, at which point the LED driver could not allocate
// its RMT buffer and the WebSocket dropped every five seconds.
constexpr size_t kHeapReserveOffline = 110 * 1024;
constexpr size_t kHeapReserveWifi = 170 * 1024;
// Open .m5a file handles kept alive (SD is mounted with max_files = 8).
constexpr uint8_t kOpenFileCacheSlots = 5;

// ------------------------------------------------------------------- sd ----
constexpr uint8_t kSdCsPin = 4;
constexpr uint32_t kSdFreqHz = 20000000;   // shared SPI bus: 20 MHz is safe
constexpr uint8_t kSdMaxOpenFiles = 8;
constexpr const char* kAssetRoot = "/companion";

// -------------------------------------------------------------- character --
constexpr uint32_t kBlinkMinMs = 2600;
constexpr uint32_t kBlinkMaxMs = 6400;
constexpr uint32_t kBlinkFrameMs = 32;     // per drawn blink frame
constexpr uint32_t kDoubleBlinkPercent = 22;
constexpr uint32_t kSaccadeMinMs = 1400;
constexpr uint32_t kSaccadeMaxMs = 5200;
constexpr uint32_t kSwayPeriodMs = 4200;   // idle breathing cycle
constexpr uint8_t  kSwayFrames = 8;
constexpr uint32_t kIdleToSleepyMs = 180000;
constexpr uint32_t kSleepyToSleepMs = 300000;
// Idle one-shots keep the companion lively without constantly repainting the
// full screen. Each choice returns to the normal 30 Hz eye/mouth renderer.
constexpr uint32_t kAmbientGestureMinMs = 7000;
constexpr uint32_t kAmbientGestureMaxMs = 17000;

// ----------------------------------------------------------------- audio ---
constexpr uint32_t kAudioSampleRate = 16000;
constexpr size_t kAudioSamplesPerChunk = 320;   // 20 ms
constexpr size_t kAudioBytesPerChunk = kAudioSamplesPerChunk * sizeof(int16_t);
constexpr uint8_t kMicQueueDepth = 8;
constexpr uint8_t kPlaybackQueueDepth = 24;     // ~480 ms jitter buffer
constexpr uint8_t kPlaybackPrerollChunks = 4;   // ~80 ms before first output
// The M5GO's electret feeds the ADC at a low level; M5Unified multiplies
// what it reads by this before handing it over.
constexpr uint8_t kMicMagnification = 16;
constexpr uint8_t kSpeakerVolume = 150;
constexpr uint8_t kVisemeCount = 8;

// --------------------------------------------------------------- network ---
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWsReconnectMs = 3000;
constexpr uint32_t kWsPingIntervalMs = 15000;
constexpr uint16_t kDefaultServerPort = 8765;
constexpr const char* kDefaultServerPath = "/m5companion";
constexpr uint32_t kProvisioningTimeoutMs = 300000;

// ------------------------------------------------------------------ leds ---
constexpr uint8_t kLedPin = 15;
constexpr uint8_t kLedCount = 10;
constexpr uint8_t kLedBrightness = 40;

// ----------------------------------------------------------------- input ---
constexpr uint32_t kBtnHoldMs = 600;
constexpr uint32_t kFactoryResetHoldMs = 3500;
constexpr uint32_t kShakeCooldownMs = 900;
constexpr uint32_t kPickupCooldownMs = 1500;

}  // namespace appcfg
