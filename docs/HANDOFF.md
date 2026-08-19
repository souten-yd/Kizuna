# Where this is, and what to pick up next

Written at the end of a long session that took the M5GO from "boots and
animates" to "talks to a NAS over Wi-Fi", and left one thing broken.

## The one blocker: the microphone runs 12-24x too slow

`M5.Mic.record()` returns valid audio, never refuses, and never errors - it
just takes about half a second per 320-sample chunk instead of 20 ms. So a
five second press produces a fifth of a second of speech, the server sees an
utterance too short to transcribe, and nothing happens.

Measured with `mictest` on the serial console, which records straight from the
driver with the state machine, the queue and the network out of the way:

| Configuration | Effective rate | Worst `record()` |
|---|---|---|
| `over=2 dma=128x8 mag=16` (M5Unified default) | 695 Hz | 589 ms |
| `over=1` | 1355 Hz | 363 ms |
| `over=1 dma_len=512` | 1244 Hz | 484 ms |
| `over=1 dma_len=1024 count=4` | 1121 Hz | 905 ms |

Wanted: 16000 Hz. Halving the oversampling doubles the rate and nothing else
moves it much, so the cost is not in the oversampling - it is in the ADC path
itself.

What is already known and ruled out:

- The configuration *is* being applied. The device reports
  `pin=34 adc=1 rate=16000 mag=16 over=2 i2s=0 dma=128x8` after `begin()`.
- It is not the queue and not the network: `dropped: 0`, and the uplink
  reports `0 sends failed`.
- M5Unified has **no microphone configuration for `board_M5Stack`** - the Core
  alone has none, and the M5GO's electret on the base board is something the
  application has to set up. Before this session that setup was missing
  entirely, and `M5.Mic.begin()` was starting a driver aimed at unconnected
  pins. Adding it is what took the rate from ~2 chunks/second to ~4; it did
  not get it to 50.
- The signal itself is clipping: `peak: 32752` of 32767 with `magnification =
  16`. Whatever fixes the rate, that wants dropping to about 4.

Where to look next, roughly in order:

1. Whether `M5.Mic.record()` is meant to be polled rather than called
   back-to-back - M5Unified also exposes `isRecording()`, and the two-buffer
   hand-off in `AudioManager::serviceCapture` may be fighting it.
2. The ESP32's I2S-ADC mode: the sample rate for ADC capture is not set the
   same way as for a real I2S microphone on this chip, and the driver may be
   running at a rate the config does not describe.
3. Reading the ADC directly with `analogRead` on a timer, bypassing I2S. The
   rate needed is only 16 kHz, which is well within reach, and it would free
   I2S0 for the speaker - the whole path is half duplex today only because
   ADC capture and the speaker's DAC both need I2S port 0.

`mictest [chunks] [over_sampling] [dma_len] [dma_count] [magnification]` takes
all five, so the next round of experiments needs no reflash.

## What works

**The voice loop, over Wi-Fi, with audio injected from the host.** Microphone
aside, every other stage is verified on hardware: the device connects, the
server transcribes, Qwen3 answers, Piper synthesises, and the M5GO speaks.

```bash
cd /data1tb/M5Companion
.venv/bin/python server/companion_server.py --host 0.0.0.0 --port 8766 \
    --qnap http://192.168.68.57:11435 --language ja
```

Port 8766 rather than 8765, which ControlDeck's own backend already has. The
device is at 192.168.68.66 and points at 192.168.68.200:8766; change that in
`build/sd/companion/config/device.json` and `tools/push_sd.py --only config`.

To drive it without the microphone:

```bash
.venv/bin/python tools/play_wav.py <file.wav> --level 0.35   # speaker only
.venv/bin/python tools/usb_link.py --url ws://127.0.0.1:8766/m5companion --kick 1.5
```

## What was fixed, and what it looked like before

Each of these presented as "the audio is gritty", and each had a different
cause. They are worth remembering as a set, because the next unexplained noise
is unlikely to be any of them.

- **The server sent faster than real time.** 16 ms per 20 ms chunk saturated
  the device's 480 ms jitter buffer after 2.4 seconds, and every chunk after
  that was dropped. Inaudible on a one-second beep; ruinous on a seven-second
  sentence.
- **`playRaw` was handed a stack buffer.** M5Unified's playback is
  asynchronous: it records the pointer and the mixer reads it later, by which
  time the next chunk had overwritten it. The device was playing freed memory.
- **22050 to 16000 without a filter.** Everything above 8 kHz folded back into
  the band. Now done on the NAS, behind a windowed sinc, by asking for the
  `m5go` voice profile.
- **Clipping.** Piper's output peaks at 99% of full scale and the M5Stack Core
  puts 8x of gain in front of its DAC. The `m5go` profile returns 0.12.

And two that were not audio at all:

- **Two heartbeats.** The server pinged every 20 seconds and the device
  pinged every 15 with a 3 second deadline. Each teardown leaked heap; after
  enough of them the handshake itself stopped completing, and the device
  reconnected every 5 seconds for ever.
- **A state that was never sent.** An utterance under 0.25 seconds was
  discarded without telling the device, which then sat in LISTENING looking
  hung.

## The artwork, separately

Kizuna's base face arrived without eyes, brows or mouth, registered to within
1% of the head width, and the new eyes composite onto it cleanly - the
smearing that started this is gone. Two things are outstanding:

- The remaining eleven base poses. `docs/CODEX_BASE_PROMPT.md` has the prompt;
  the requirement that matters is that the body is pixel-identical across all
  twelve, which is worth about six frames a second.
- A `hair_front` layer. This character's bangs cross his eyes, and 57% of the
  eye rectangle is hair, so the eyes have to be drawn *under* them. See
  `docs/ASSET_BRIEF.md` §3.4.

Eye placement is anchored in `assets/kizuna/eye_anchor.json` - read off a
magnified tile by eye, because every automatic estimator put it about 20 px
low. `tools/normalise_eyes.py` puts a delivered file onto that anchor.
