// M5Companion - global compile-time configuration
//
// All tunables live here so board revisions and SD cards of different speed
// classes can be adapted without touching subsystem code.
#pragma once

#include <Arduino.h>

#include "Board.hpp"

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
//
// The console added mDNS and the OTA listener to that bill - together about
// another 15 KB, and both only exist once Wi-Fi is up, so they come out of the
// same figure. The cost is tile cache, which is frames per second; the
// alternative is the heap exhaustion above, which is the whole device. If a
// unit needs the frames back, `web_server: false` in device.json takes all of
// it away again - and `min_heap` on the status page is how to tell whether it
// needs to.
constexpr size_t kHeapReserveOffline = 110 * 1024;
constexpr size_t kHeapReserveWifi = 185 * 1024;
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
// 320 ms of microphone, against 160 ms before. The queue only has to cover the
// worst pause between two turns of the loop that drains it, and a websocket
// send that has to wait for the socket is exactly such a pause. Overflowing it
// throws away speech: the device reported 1995 chunks dropped - forty seconds
// of an utterance - on a link that was otherwise healthy.
constexpr uint8_t kMicQueueDepth = 16;
constexpr uint8_t kPlaybackQueueDepth = 24;     // ~480 ms jitter buffer
constexpr uint8_t kPlaybackPrerollChunks = 4;   // ~80 ms before first output
// The microphone is read from the ADC directly rather than through I2S.
// ESP32's I2S built-in ADC mode delivers roughly a eighteenth of the sample
// rate it is configured with - measured here as 16000 asked for and 672
// achieved, and reported upstream as arduino-esp32 #6738, closed as not
// planned. M5Stack's own M5GO microphone example does the same thing this
// does: analogRead on GPIO34, at 12 kHz.
//
// 12 kHz rather than 16 on M5GO: one conversion plus the loop overhead is
// comfortably under 83 us and uncomfortably close to 62. CoreS3 has a real
// ES7210 codec and therefore records at the protocol's native 16 kHz.
constexpr uint32_t kMicSampleRate = M5COMPANION_BOARD_CORES3 ? 16000 : 12000;
constexpr size_t kMicSamplesPerChunk = kMicSampleRate / 50;   // 20 ms
// 12 bits to 16, as in M5Stack's example. The electret has no preamp, so this
// is where the level comes from.
constexpr uint8_t kMicGainShift = 4;
constexpr uint8_t kSpeakerVolume = 150;

// Five steps the B button walks up and then wraps. Spread across the usable
// part of the range rather than evenly across 0-255: the amplifier is fixed
// gain and the DAC is eight bits, so the low end runs out of bits before it
// runs out of numbers.
constexpr uint8_t kVolumeSteps[] = {60, 100, 140, 180, 220};
constexpr uint8_t kVolumeStepCount = sizeof(kVolumeSteps) / sizeof(kVolumeSteps[0]);
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

// ----------------------------------------------------------------- power ---
// What changes when the cable comes out.
//
// The classic Core runs from a small cell through a boost converter, and the
// radio at full power draws a spike of a few hundred milliamps every time it
// sends - fifty times a second while an utterance is going up. A cell with age
// on it has internal resistance, and resistance times that current is a sag.
// The CPU's own brownout detector sits at the lowest of its eight thresholds
// in this build, so the rail can fall far enough to upset the SD card and the
// radio long before anything resets and says why.
//
// So on battery the device gives up the three things it can most afford to:
// range, the LED bar, and some backlight.
constexpr int8_t kBatteryTxPowerDbm = 13;   // from ~20; about a third of the current
constexpr uint8_t kBatteryBrightnessCap = 110;

// The microphone starts, the face changes, and the radio starts sending, all
// on the same button press. The face change is a burst of SD reads and SPI
// writes; the radio is a burst of transmit spikes. Overlapping them is asking
// the supply for both at once, at the exact moment the user is least willing
// to see a failure.
//
// So the audio waits a moment before it goes up. Nothing is lost: the capture
// queue holds 320 ms and this is half of that, so the delayed chunks are sent,
// just slightly later. The reply arrives a fraction of a second further out
// and no one can tell.
constexpr uint32_t kUplinkHoldoffMs = 150;

// Longer than the longest thing the loop is allowed to block on. See the note
// where this is applied: the web server's response write can legitimately take
// five seconds, which is what the default happens to be set to.
constexpr uint32_t kLoopWatchdogSeconds = 10;

// -------------------------------------------------------------- recovery ---
// Failed boots before the small recovery application in the factory partition
// is handed control. Later than the other rungs because it is the one that
// writes flash; see App::escalateRecovery.
constexpr uint8_t kRecoveryAppStreak = 5;

// After the backup has been attempted, how much longer the device has to keep
// running before the rollback is given up. The backup is the heaviest thing
// this firmware does to itself - a megabyte and a half read out of flash and
// written to the card - so surviving it, and then a little more, is the last
// piece of evidence worth waiting for.
constexpr uint32_t kSettleAfterBackupMs = 10000;

// ----------------------------------------------------------------- input ---
constexpr uint32_t kBtnHoldMs = 600;
constexpr uint32_t kFactoryResetHoldMs = 3500;
constexpr uint32_t kShakeCooldownMs = 900;
constexpr uint32_t kPickupCooldownMs = 1500;

}  // namespace appcfg
