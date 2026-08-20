# ADC microphone runs at a fixed ~1.2 kHz on classic ESP32

A report ready to file against [m5stack/M5Unified](https://github.com/m5stack/M5Unified),
with the register dump that shows the cause. Reproduced on an M5GO IoT Kit -
classic ESP32, no PSRAM - with the electret on GPIO34.

## Symptom

`Mic_Class` with `use_adc = true` delivers audio at a small fraction of the
configured `sample_rate`, silently. `record()` returns true, no error is
logged, and the samples are valid - there are just far too few of them. A five
second push-to-talk press produced 0.2 seconds of speech.

| `over_sampling` | `sample_rate` asked for | Delivered |
|---|---|---|
| 2 (the default) | 16000 | 695 Hz |
| 1 | 16000 | 1355 Hz |

Halving the oversampling doubles the delivered rate and nothing else moves it,
which is the shape of a raw stream fixed at ~1.3 kHz being averaged down.

## Cause

`Mic_Class.cpp`:

```cpp
uint32_t bits = (self->_cfg.use_adc) ? 1 : 16;
uint32_t div_m = 8;
calcClockDiv(&div_a, &div_b, &div_n, PLL_D2_CLK / (bits * div_m),
             self->_cfg.sample_rate * oversampling);
```

With `use_adc`, `bits` is 1, so the base clock handed to `calcClockDiv` is
`80 MHz / 8 = 10 MHz`. Reaching 32 kHz from there needs a divider of 312.5.

`calcClockDiv` cannot express it:

```cpp
uint32_t save_n = 255;
uint32_t save_a = 63;
uint32_t save_b = 62;
if (targetFreq)
{
  float fdiv = (float)baseClock / targetFreq;
  uint32_t n = (uint32_t)fdiv;
  if (n < 256)
  {
    ...            // the entire search lives inside this branch
  }
}
```

`n` is 312, the branch is skipped, and the initial values are returned
unchanged. The divider is not clamped or reported - it is simply never
computed.

Because the base clock is 10 MHz and `n` is capped at 255, **no ADC sample
rate below about 39 kHz can be represented**, which is every rate a
microphone would be used at.

## The register dump

Read back from `I2S0` after `Mic.begin()`, on hardware:

```json
{"over":2,"clkm_div_num":255,"clkm_div_a":63,"clkm_div_b":62,
 "rx_bck_div_num":8,"rx_bits_mod":16,
 "divider_needed":312.5,"i2s_clk_hz":39065,"raw_per_bck32_hz":1221}

{"over":1,"clkm_div_num":255,"clkm_div_a":63,"clkm_div_b":62,
 "rx_bck_div_num":8,"rx_bits_mod":16,
 "divider_needed":625.0,"i2s_clk_hz":39065,"raw_per_bck32_hz":1221}
```

`clkm_div_num`, `clkm_div_a` and `clkm_div_b` are 255/63/62 - the initial
values, untouched. The two configurations ask for divisors of 312.5 and 625
and get the same clock, which is the clearest statement of the bug: the
divider does not vary with what is requested.

39065 Hz through `rx_bck_div_num = 8` and `rx_bits_mod = 16` - 32 BCK per
frame - is 1221 Hz, against 1355 measured at `over_sampling = 1`. The
remaining 10% is not worth chasing; the mechanism is not in doubt.

## Suggested fix

Give the ADC path a divider it can reach. `div_m` is 8 to keep the CoreS3's
MCLK accurate, which does not apply when `use_adc` is set and there is no
MCLK; raising it for that case brings the base clock down into range:

```cpp
uint32_t div_m = self->_cfg.use_adc ? 64 : 8;
```

That makes the base 1.25 MHz, and 32 kHz needs a divider of 39 - comfortably
inside 255.

Separately, `calcClockDiv` returning silently when the divider is out of range
is what made this take a day to find. Clamping and logging, or returning a
failure the caller can act on, would have made it a minute.

## Not the cause

Ruled out on hardware, in case they come up:

- **`dma_buf_len` / `dma_buf_count`.** Raising `dma_buf_len` to 1024 made it
  worse in exactly the way a slow stream predicts: `record()` took 905 ms,
  against `1024 / 1121 Hz = 913 ms` to fill one buffer. The DMA is waiting on
  the clock, not causing it.
- **Polling `record()`.** M5Unified's own microphone example calls it in a
  loop; the bottleneck is below that API.
- **`adc_ll_digi_set_clk_div(16)`.** That matches ESP-IDF's own default for
  continuous ADC on this part.
- **The ADC itself.** Reading the same pin with `adc1_get_raw` on a timed loop
  gives 12036 Hz against 12000 requested, on the same board, in the same
  firmware.

## Workaround

Read the ADC directly. M5Stack's own
[M5GO microphone example](https://docs.m5stack.com/en/arduino/m5go_kit/mic)
does this rather than using `Mic_Class`, which is probably not a coincidence.
