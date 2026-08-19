# M5Companion

A character companion that runs on an M5GO IoT Kit: it blinks, follows you
with its eyes, reacts to being picked up, listens when you hold a button, and
answers out loud with its mouth moving.

The M5GO is the body - display, microphone, speaker, buttons, IMU, LEDs. A
server on your network is the head - speech recognition, a language model,
speech synthesis. The device keeps working when the server does not.

```
        hold A ──▶ LISTENING ──▶ THINKING ──▶ SPEAKING ──▶ IDLE
                      │              │            │
                   mic 16 kHz      STT+LLM      TTS + lip sync
```

## Hardware

M5GO IoT Kit v2.5 / v2.6 / v2.7 - classic ESP32, 320x240 ILI9341, analog mic
on GPIO34, 1 W speaker on GPIO25, MPU6886, 10 SK6812 LEDs on GPIO15, microSD.
No PSRAM. A microSD card of any size, **formatted FAT32** - see
[docs/SD_SETUP.md](docs/SD_SETUP.md), because larger cards commonly arrive as
exFAT and will not mount until reformatted.

## Quick start

```bash
# 1. build and flash
pio run -e m5go -t upload

# 2. validate and build the Kizuna character pack
python tools/validate_kizuna_sources.py
python tools/build_kizuna_pack.py --out build/sd

# 3. put it on the card - over USB, no card removal needed
python tools/push_sd.py

# 4. run a server (nothing to install for this one)
python server/companion_server.py --mock --tts tone
```

Point the device at the server by editing `/companion/config/device.json` on
the card, or hold **B+C through power-on** for a Wi-Fi setup portal.

With no Wi-Fi configured the companion still boots, animates and reacts - it
just says OFFLINE in the status bar.

## Controls

| Control | Action |
|---|---|
| Hold **A** | Push to talk |
| Release **A** | Send the utterance |
| **B** click | Mute / unmute |
| **B** hold | Debug overlay (fps, heap) |
| **C** click | Cycle screen brightness |
| **C** hold | Sleep / wake |
| **B+C** 3.5 s | Clear settings and reboot |
| **B+C** at power-on | Wi-Fi setup portal |

Tilting the device moves its gaze. Shaking it startles it and can play a comic
one-shot. Picking it up gets its attention. None of that needs the server.

## Server

Backends are swappable; the interesting part of this project is on the device.

```bash
# fully local: whisper on the CPU, a small model via Ollama, espeak-ng
python server/companion_server.py --stt whisper --llm ollama \
    --ollama-model qwen3:0.6b --tts espeak --language ja

# Claude for the language model
export ANTHROPIC_API_KEY=...
python server/companion_server.py --stt whisper --llm claude --tts piper \
    --piper-voice /path/to/voice.onnx

# no models at all - echoes what it heard, beeps the reply
python server/companion_server.py --mock --tts tone
```

Test the whole pipeline without hardware:

```bash
python tools/fake_m5go.py --url ws://127.0.0.1:8765/m5companion
```

### Why the language model is not on the device

A 0.6 B model at 4-bit is ~350 MB of weights against 16 MB of flash and
520 KiB of RAM, and every token needs the whole file read back over SPI -
roughly nine seconds per token even if it fit. `--llm ollama` puts the model
one hop away instead.

## Character packs

Packs live on the SD card and sit side by side; the device picks one by name.
Which artwork means what is data, not code - see
[docs/ASSET_SPEC.md](docs/ASSET_SPEC.md) to add a character and
[docs/KIZUNA_PACK.md](docs/KIZUNA_PACK.md) for the Kizuna expression/loop map.

Three ways to manage them: `tools/push_sd.py` over USB (incremental, CRC
compared), `http://<device-ip>/` over Wi-Fi (upload, delete, switch), or a
card reader with `tools/deploy_sd.sh`.

## How it stays smooth

The LCD and the SD card share one SPI bus, so every tile crosses it twice -
read from the card, written to the panel. Around 850 KB/s of artwork fits.
That budget is measured at boot rather than assumed, and it is why:

- nothing is decoded at runtime; frames are raw RGB565 in the panel's own byte
  order,
- eye/mouth changes stay in small tiles and can continue at the 30 Hz display
  loop,
- a full-screen repaint is spread progressively across several ticks,
- full-screen comic gestures are short one-shots capped at 5 fps, then the
  normal layered face is restored.

A 32 GB card is therefore useful for storing many poses, expressions and
one-shots; it does not increase the instantaneous bandwidth of the shared SPI
bus.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/TUNING.md](docs/TUNING.md), and [docs/KIZUNA_PACK.md](docs/KIZUNA_PACK.md).

## Layout

```
include/            shared types, the .m5a format, protocol constants
src/app/            event bus, state machine, serial console, orchestration
src/character/      blink, gaze and viseme timing
src/display/        SD, asset pack, tile cache, renderer, display task
src/audio/          half-duplex mic and speaker
src/network/        Wi-Fi, WebSocket, setup portal, pack web server
src/device/         LEDs, power
src/storage/        NVS and SD configuration
assets/characters/  one recipe per character
assets/source/      the artwork sheets
tools/              packer, USB push, SD deploy, fake device, monitor
server/             the speech and language server
```

## Status

The original hardware path is verified on M5GO: SD mount at 1.47 MB/s, 30 Hz
render loop, blink and viseme tiles, USB pack push, and the full server round
trip. Kizuna builds on that path by adding a larger semantic expression set and
short full-screen gesture playback; see the PR/testing notes for validation of
those changes.
