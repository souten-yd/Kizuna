# M5Companion

A character companion that runs on an M5Stack CoreS3: it blinks, follows you
with its eyes, reacts to being picked up, listens when you hold a button, and
answers out loud with its mouth moving.

The CoreS3 is the body - display, touch controls, microphone, speaker and IMU. A
server on your network is the head - speech recognition, a language model,
speech synthesis. The device keeps working when the server does not.

```
        hold A ──▶ LISTENING ──▶ THINKING ──▶ SPEAKING ──▶ IDLE
                      │              │            │
                   mic 16 kHz      STT+LLM      TTS + lip sync
```

## Hardware

M5Stack CoreS3 - ESP32-S3, 320x240 ILI9342C touch display, ES7210 microphone
ADC, AW88298 speaker amplifier, BMI270 IMU, 16 MB flash and 8 MB PSRAM. The
previous M5GO build remains available as the `m5go` PlatformIO environment.
A microSD card of any size, **formatted FAT32** - see
[docs/SD_SETUP.md](docs/SD_SETUP.md), because larger cards commonly arrive as
exFAT and will not mount until reformatted.

## Quick start

```bash
# 1. build and flash the CoreS3 firmware
pio run -e cores3 -t upload

# 2. validate and build the Kizuna character pack
python tools/validate_kizuna_sources.py
python tools/build_kizuna_pack.py --out build/sd

# 3. put it on the card - over USB, no card removal needed
python tools/push_sd.py

# 4. run a server (nothing to install for this one)
python server/companion_server.py --mock --tts tone
```

Point the device at the server by editing `/companion/config/device.json` on
the card, or hold the **gear area through power-on** for a Wi-Fi setup portal.

With no Wi-Fi configured the companion still boots, animates and reacts - it
just says OFFLINE in the status bar.

## Controls

CoreS3 has a six-icon touch bar down the right edge of the screen.

| Icon | Action |
|---|---|
| Microphone, hold | Push to talk; release to send |
| Speaker | Volume, five steps, wrapping |
| Muted speaker | Mute / unmute |
| Sun | Cycle screen brightness |
| Moon | Sleep / wake |
| Gear, hold 3.5 s | Clear settings and reboot |
| Gear at power-on | Wi-Fi setup portal |

The legacy M5GO environment retains the original A/B/C button mappings.

Tilting the device moves its gaze. Shaking it startles it and can play a comic
one-shot. Picking it up gets its attention. None of that needs the server.

## Server

Backends are swappable; the interesting part of this project is on the device.

```bash
# fully local: whisper on the CPU, a small model via Ollama, espeak-ng
python server/companion_server.py --stt whisper --llm ollama \
    --ollama-model qwen3:0.6b --tts espeak --language ja

# a model on the NAS: any OpenAI-compatible endpoint, e.g. QnapAssistant
python server/companion_server.py --stt whisper --llm openai \
    --openai-base-url http://192.168.68.57:11435/v1 --tts espeak --language ja

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

## Debugging without the cable

The failures worth chasing are the ones that happen with the USB lead out - a
reboot on battery, a task that stops, a link that drops overnight - and a
device with no cable used to have no voice at all. Now it has a web console.

```bash
tools/m5net.py status     # one snapshot: state, heap, battery, uplink, drops
tools/m5net.py log -f     # the firmware log, followed, over Wi-Fi
tools/m5net.py boots      # how the last eight boots ended, kept in flash
tools/m5net.py serve      # reflash it from here, no cable
```

The same data is at `http://<device-ip>/` for a browser. Reflashing also works
straight from the build system:

```bash
pio run -e cores3_ota -t upload --upload-port <device-ip>
```

The one that answers the original question is `boots`: it survives losing
power, so a device that vanished on battery can be asked what it was doing
when it went.

An update that does not work undoes itself. A new image runs on probation and
the bootloader reverts it unless it stays healthy for a minute; a firmware that
crashes on every boot gets the other slot, then safe mode. The M5GO partition
layout additionally supports a small factory recovery application; CoreS3 uses
its standard dual-app partition layout without that M5GO-only layer. Backups
are written to the card the first time each build proves itself, so "known
good" means it ran.

```bash
tools/m5net.py backups    # what is there to fall back to
tools/m5net.py restore    # put the known-good one back
```

The battery load-shedding and IP5306 boost-hold workarounds are M5GO-only. The
CoreS3 uses an AXP2101 PMIC, has no M5GO LED bar, and keeps full Wi-Fi transmit
power and configured backlight level on battery.

See [docs/POWER.md](docs/POWER.md) for that, and
[docs/REMOTE_DEBUG.md](docs/REMOTE_DEBUG.md) for the rest - including the IP5306,
the power-bank chip in the classic Core that switches itself off under a light
load, and is usually the answer to a device that vanishes when you unplug it.

## Debugging over USB

The device can reach the server through the cable that flashes it, which is
worth having because a freshly flashed board has no Wi-Fi credentials and
provisioning them is the slowest step in the loop:

```bash
python tools/usb_link.py --url ws://127.0.0.1:8765/m5companion
python tools/usb_link.py --url ws://... --kick 1.5   # and fake holding A
```

The server is unchanged and cannot tell the difference - see
[docs/PROTOCOL.md](docs/PROTOCOL.md). Wi-Fi remains how the finished thing
works; both transports can be live at once.

### Why the language model is not on the device

A 0.6 B model at 4-bit is ~350 MB of weights against 16 MB of flash and 8 MB
of PSRAM. It still does not fit; `--llm ollama` puts the model one hop away
instead.

## Character packs

Packs live on the SD card and sit side by side; the device picks one by name.
Which artwork means what is data, not code - see
[docs/ASSET_SPEC.md](docs/ASSET_SPEC.md) to add a character and
[docs/KIZUNA_PACK.md](docs/KIZUNA_PACK.md) for the Kizuna expression/loop map.

Three ways to manage them: `tools/push_sd.py` over USB (incremental, CRC
compared), the Packs tab at `http://<device-ip>/` over Wi-Fi (upload, delete,
switch), or a card reader with `tools/deploy_sd.sh`.

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
src/network/        Wi-Fi, WebSocket, setup portal, web console, OTA
src/device/         LEDs, power
src/diag/           the log ring, the boot history, the recovery ladder
src/recovery/       the recovery application (a separate, smaller firmware)
src/storage/        NVS and SD configuration
assets/characters/  one recipe per character
assets/source/      the artwork sheets
tools/              packer, USB push, SD deploy, fake device, monitor, m5net
server/             the speech and language server
```

## Status

The original M5GO hardware path remains buildable. CoreS3 is now the default
target; its board build is verified, while display, touch, microphone, speaker
and end-to-end server behaviour require the post-flash hardware smoke test.
