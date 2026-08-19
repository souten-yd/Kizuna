# Tuning

Every number worth changing is in `include/AppConfig.hpp` or in the character
recipe. This is what each one costs.

## The budget

Read it off the device rather than guessing:

```bash
python tools/push_sd.py --info
```

```json
{"fps":30.3,"sd_bps":1475329,"budget_bps":852450,"drawn_bps":0,
 "cache_slots":4,"cache_hits":22,"cache_misses":35,"heap":94016}
```

- `sd_bps` is measured at boot on a real asset file.
- `budget_bps` is what `FrameBudget` will let through: 26 ms of bus time per
  33 ms tick, priced at the SD read plus the LCD write for each byte.
- `drawn_bps` is what the renderer actually moved in the last second. Idle is
  near zero; continuous lip sync is around 150 KB/s.

If `drawn_bps` sits at `budget_bps`, the animation is bus-bound and layers are
being deferred - shrink a rectangle or lower a rate.

## Frame rates

| Constant | Default | Effect |
|---|---|---|
| `kDisplayTickHz` | 30 | How often the renderer wakes. Raising it does not create bandwidth. |
| `kMaxTickBusMs` | 26 | Bus time per tick. Above ~28 the loop cannot keep 30 Hz. |
| `kBlinkFrameMs` | 32 | Per drawn blink stage. Eight stages ≈ 256 ms. |
| `kBlinkMinMs`/`MaxMs` | 2600/6400 | Blink interval. |
| `kSaccadeMinMs`/`MaxMs` | 1400/5200 | How often an idle gaze wanders. |

## Memory

The ESP32 in an M5GO has 520 KiB of SRAM and no PSRAM. After the framework,
Wi-Fi and the WebSocket client there is roughly 100 KiB of usable heap.

| Constant | Default | Notes |
|---|---|---|
| `kTileCacheBytes` | 48 KiB | An optimisation, never a requirement - a miss streams from SD instead. Sized so the Wi-Fi stack, which starts later, still fits. |
| `kBandBytes` | 10 KiB | The renderer's entire drawing working set. |
| `kPlaybackQueueDepth` | 24 | ~480 ms of audio jitter buffer. |

`min_heap` in `--info` is the low-water mark since boot. If it drops under
about 40 KiB, shrink the tile cache first.

## SD card

`kSdFreqHz` defaults to 20 MHz. The firmware retries at 10 and 4 MHz on
failure, so a marginal connector still works - slowly. If `sd_bps` comes back
under 1 MB/s, that ladder probably fell through; try a different card before
changing code.

## Audio

`kAudioSamplesPerChunk` is 320 (20 ms) and matches the server. Changing it
means changing both. `kPlaybackPrerollChunks` (4) trades startup latency for
robustness against network jitter; below 2, a single late packet is audible.

## Tile geometry

In the character recipe, not the firmware. Each tile crosses the bus twice per
draw:

```
bytes = width * height * 2
cost at 30 Hz = bytes * 30    (compare against budget_bps)
```

The shipped eye rectangle is 128x44 = 11,264 bytes, or 338 KB/s if it changed
every tick. It does not - blinks are occasional - but a rectangle sized as if
it were will stutter the moment something else needs the bus.
