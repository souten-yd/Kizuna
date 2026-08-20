# Where M5Unified's idea of this board and the actual M5GO differ

M5Unified identifies an M5GO IoT Kit as `board_M5Stack` - which is correct, it
*is* a Core - and then knows nothing about the M5GO Base bolted underneath it.
Everything the base adds is the application's problem, and two of those things
are actively hostile to what the Core wants to do.

The microphone was the first of these to bite, and it cost a day. This is the
sweep for the rest, done after the fact.

## 1. There is no microphone configuration — confirmed, cost a day

`board_M5Stack` sets `spk_cfg` and no `mic_cfg` at all. The Core has no
microphone; the M5GO Base has an electret on GPIO34. `M5.Mic.begin()` without
setting that up starts a driver aimed at unconnected pins, `record()` keeps
returning true, and about two chunks a second come back.

Worse, configuring it correctly does not help either: `Mic_Class`'s ADC clock
path is broken on classic ESP32 - see
[M5UNIFIED_ADC_MIC_BUG.md](M5UNIFIED_ADC_MIC_BUG.md). The firmware now reads
the ADC directly, which is also what M5Stack's own M5GO example does.

**Status: worked around.** 12036 Hz measured against 12000 requested.

## 2. GPIO15 is fought over — the LEDs against Wi-Fi

M5Unified's very first act for this board is:

```cpp
case board_t::board_M5Stack:
  // Countermeasure to the problem that GPIO15 affects WiFi sensitivity
  // when M5GO bottom is connected.
  m5gfx::pinMode(GPIO_NUM_15, m5gfx::pin_mode_t::output);
  m5gfx::gpio_lo(GPIO_NUM_15);
```

GPIO15 is where the M5GO Base's ten SK6812s live. The library deliberately
holds it low to protect Wi-Fi; this firmware then hands the pin to
Adafruit_NeoPixel, which drives it push-pull.

M5Stack's own documentation says to use **`pinMode(15, OUTPUT_OPEN_DRAIN)`**
for the LED on this base, and the community has
[a thread](https://community.m5stack.com/topic/1452/wifi-connection-failure-when-using-go-base-bottom)
reporting that stock ESP32 Wi-Fi examples will not associate at all with a GO
base attached, across several Cores and bases.

That is worth taking seriously here: this session saw repeated `AUTH_FAIL` and
`4WAY_HANDSHAKE_TIMEOUT` on a network the same board later sat on for an hour
without dropping. RSSI reads -49 to -60 a metre from the router, which is poor
for that distance.

**Status: not addressed.** Nothing has been changed yet, and the interaction
has not been isolated. The cheap experiment is to run the link with the LEDs
never driven and compare association attempts and RSSI.

## 3. The battery percentage is five values, not a hundred

`Power_Class` uses the IP5306 on this board, and its level register carries
four bits of state:

```cpp
switch (data >> 4) {
  case 0x00: return 100;
  case 0x08: return 75;
  case 0x0C: return 50;
  case 0x0E: return 25;
  default:   return 0;
}
```

So the status bar's "+100%" is a bucket, not a measurement, and it will sit at
100 for most of a charge and then step. Worse for us: a failed I2C read returns
**-1**, and `PowerManager` does `constrain(level, 0, 100)`, which turns that
into a confident **0%** - indistinguishable from a flat battery.

**Status: cosmetic, but the -1 needs handling.** The reading should be shown as
one of five states, or as a bar rather than a number, and a read failure should
show as unknown rather than as empty.

## 4. The SD card speed note has a parenthesis in it

The same board case raises the drive current on the SPI pins, with:

> This allows SunDisk SD cards to communicate at 20 MHz. **(without M5GO
> bottom.)**

This firmware mounts at 20 MHz and gets 1.45 MB/s, which is fine - but the
library's own note says the 20 MHz figure is for a bare Core. If SD reads ever
start failing intermittently on a different card, this is the first thing to
suspect rather than the last.

**Status: works, but on the wrong side of a documented caveat.**

## 5. The speaker is the Core's, and it is an 8-bit DAC

Not a mismatch - M5Unified gets this right - but worth recording next to the
others, because the numbers explain a lot of what sounded broken:

```cpp
spk_cfg.use_dac = true;          // GPIO25, the ESP32's own DAC
spk_cfg.pin_data_out = GPIO_NUM_25;
spk_cfg.magnification = 8;       // eight times, before the DAC
spk_cfg.sample_rate *= 2;        // 96 kHz output
```

Eight bits and 8x of gain in front of them is why a reply normalised near full
scale warbles, and why the NAS returning a peak of 0.12 fixed it.

Also not a mismatch but easy to get wrong, and this firmware did:
`playRaw` does not copy the buffer it is given. The maintainer says so in
[#29](https://github.com/m5stack/M5Unified/issues/29) - "it finishes processing
without making a copy of the received data ... prepare multiple data buffers on
the user code side and use them in order", three of them. This firmware was
handing it a stack local.

## 6. Things checked and found fine

- **Display.** Autodetected as M5Stack, ILI9341, correct.
- **Buttons.** GPIO39/38/37, the Core's own, and `wasPressed`/`wasReleased`
  are the documented push-to-talk pattern.
- **IMU.** Detected over I2C rather than assumed from the board, so a v2.6
  (MPU6886) and an older unit (MPU9250) both work.
- **The microphone queue.** `xQueueSend` copies, so the lifetime trap that
  applies to `playRaw` does not apply here.

## The pattern

Every one of these comes from the same place: **M5Unified configures the Core,
and the M5GO is a Core plus a base it cannot see.** The library is not wrong to
do that - it has no way to detect the base - but it means anything the base
adds needs checking by hand, and two of those things (the microphone, GPIO15)
are cases where the library's correct behaviour for a bare Core is the wrong
behaviour for this device.

The one that is still open is GPIO15.
