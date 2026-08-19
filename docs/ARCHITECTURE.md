# Architecture

## The split

The M5GO is the body. The server is the head.

```
                     Wi-Fi / WebSocket
   ┌──────────────────────┐        ┌────────────────────────┐
   │        M5GO          │◀──────▶│   companion server     │
   │                      │        │                        │
   │  display  animation  │        │  STT → LLM → TTS       │
   │  mic      buttons    │        │  conversation history  │
   │  speaker  IMU        │        │                        │
   │  LEDs     SD card    │        └────────────────────────┘
   └──────────────────────┘
```

The device keeps everything with a deadline: 30 Hz animation, 20 ms audio
chunks, button latency. The server keeps everything that needs more than
520 KiB: speech recognition, the language model, speech synthesis.

The split is deliberate in one more way - **the device stays alive when the
server does not**. Blinks, gaze, gestures, the IMU reactions and the button
handling are all local. An unreachable server changes the status bar and the
expression; it does not turn the companion into a brick.

## Why no language model on the device

A 0.6 B parameter model at 4-bit quantisation is roughly 350 MB of weights.
The M5GO has 16 MB of flash and 520 KiB of SRAM with no PSRAM. Even ignoring
memory, every token requires reading the whole weight file, and SPI flash
tops out near 40 MB/s - about nine seconds per token. The device runs
keyword-scale models at most. `--llm ollama` puts a small model one hop away
on the LAN instead, which is as local as this architecture can get.

## Tasks and cores

```
core 0                       core 1
  Wi-Fi / lwIP                 Arduino loop  - orchestration, network, input
  AudioTask (prio 4)           DisplayTask (prio 3) - the only owner of SPI
```

`WebSocketsClient` is not thread safe, so it lives on exactly one task: the
Arduino loop. Microphone audio reaches it through a queue rather than being
sent from the audio task, and incoming PCM leaves it through another queue.

## The SPI bus is the whole design constraint

On M5GO the LCD and the microSD slot share MOSI (23), MISO (19) and SCK (18)
with separate chip selects. Two consequences drive everything else:

1. **One owner.** `DisplayTask` is the only task that may touch `M5.Display`
   or the `SD` library. Anything else that needs the card - the serial
   console, the web uploader - calls `DisplayTask::pause()` first and gets
   the bus handed over. There is no locking scheme that makes concurrent
   access safe.

2. **Every tile costs its size twice**: once read from the card, once written
   to the panel. `FrameBudget` prices exactly that, using an SD throughput
   figure measured at boot rather than assumed - a tired class-4 card and a
   UHS-I card differ by more than 3x, and the animation should degrade rather
   than stutter.

Measured on the reference unit: SD 1.47 MB/s, LCD ~4.2 MB/s, giving a budget
of about 28 KB of artwork per 33 ms tick, or 850 KB/s.

## Rendering

Nothing is decoded at runtime. Frames are stored as raw RGB565 in the
panel's own byte order, so drawing is a seek, a read and a DMA push.

```
   base      full screen, one frame per expression   150 KB
   eyes      12 slots, blink stages and gaze          11 KB each
   mouth     8 visemes                                 5 KB each
   clips     full-screen gesture animations          150 KB per frame
```

An expression change repaints the base progressively across several ticks so
a slow card cannot freeze the loop; tiles are suppressed while it is in
flight, because they would land on rows that have not been painted yet.

Between base repaints only the eye and mouth rectangles move. That is what
buys 30 Hz blinking and lip sync out of a bus that can only manage about
7 full frames a second.

## Assets

Artwork lives on the card, not in flash, which is the point of a 64 GB card:
storage is free, CPU cycles are not. See `ASSET_SPEC.md` for the pack format
and how to add a character.
