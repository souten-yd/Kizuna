#pragma once

// What differs between the boards this firmware runs on.
//
// The M5GO is an M5Stack Core: the speaker hangs off the ESP32's own 8-bit DAC
// behind a fixed-gain amplifier with no enable line, and the microphone is an
// electret read through ADC1. Every awkward thing in AudioManager is there
// because of those two facts - the DC bias tracking, the zero-level pinning,
// the 12 kHz busy-wait that exists because I2S will not clock the built-in ADC
// slowly enough. None of it applies to a board with a real codec.
//
// The CoreS3 has one: an AW88298 on the output and an ES7210 on the input, both
// I2S, both driven by M5Unified without help. It also has no A/B/C buttons -
// the front is a touch panel - and no NeoPixel bar, which lives on the M5GO
// base rather than on any Core.

#if defined(ARDUINO_M5STACK_CORES3) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define M5COMPANION_BOARD_CORES3 1
#else
#define M5COMPANION_BOARD_CORES3 0
#endif

namespace board {

// A codec takes samples over I2S at whatever rate it is asked for. A DAC and an
// ADC on GPIO pins do not.
constexpr bool kAnalogAudio = !M5COMPANION_BOARD_CORES3;

// Physical buttons under the screen.
constexpr bool kHasButtons = !M5COMPANION_BOARD_CORES3;

// The ten-pixel bar on the M5GO base.
constexpr bool kHasLedBar = !M5COMPANION_BOARD_CORES3;

// The touch panel that replaces the buttons.
constexpr bool kHasTouch = M5COMPANION_BOARD_CORES3;

}  // namespace board
