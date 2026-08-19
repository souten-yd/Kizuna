# Kizuna character pack

`kizuna` is the default M5GO companion artwork pack. It is generated from the
clean 4x4 source sheets under `assets/source/` and the recipe in
`assets/characters/kizuna.json`.

## What is in the pack

- 33 named expressions, including neutral, soft/happy/laughing, surprised,
  embarrassed, shy, pout, curious, relieved, apologetic, mischievous, peace,
  sleepy/yawning, focused, starry, skeptical, startled and cozy variants.
- 12 eye slots: five gaze directions, progressive blink stages, wide eyes and
  sleepy half/closed states.
- 8 visemes for quiet-to-wide speech plus rounded-O and closed/open smile mouths.
- 10 one-shot gesture clips whose frame lists explicitly return to a stable
  pose: nod, shake, left/right tilt, lean, look-around, idle moment, explain,
  cheer and sleepy pose.

## Build

```bash
python tools/build_kizuna_pack.py --out build/sd
python tools/push_sd.py
```

The generated directory is:

```text
build/sd/companion/packs/kizuna/
  manifest.json
  base/*.m5a
  eyes/*.m5a
  mouth/*.m5a
  clips/*.m5a
```

New installations default to `pack = kizuna`. Existing installations that
already saved `claudecode` in NVS keep that explicit choice; switch the pack
from the web pack manager or set `"pack": "kizuna"` in
`/companion/config/device.json`.

### Replaceable backgrounds

Kizuna keeps its source sheets on a pure-white background and composites the
scene while building the pack. The ESP32 therefore never has to alpha-blend a
large character at runtime. To make a room/home/desk variant, build the same
artwork against another image and give it a different pack name:

```bash
python tools/build_kizuna_pack.py \
  --background assets/backgrounds/home.jpg \
  --background-theme light \
  --name kizuna-home \
  --out build/sd
```

Store several variants side by side on the 32 GB card and switch them with the
existing web pack manager. This changes the background without regenerating the
character source art or adding runtime decoding/compositing load.

## Animation policy

The 32 GB card is intentionally treated as plentiful storage, but storage size
is not the limiting resource while drawing. The LCD and microSD share one SPI
bus, and a complete 320x240 RGB565 frame is 153,600 bytes. Kizuna therefore
uses two animation lanes:

1. **Continuous 30 Hz lane** — only the 128x44 eye tile and 64x40 mouth tile
   change for blink, gaze and lip sync. These remain responsive during speech.
2. **Comic one-shot lane** — full-screen illustrated gestures play only while
   idle or on explicit physical reactions. Playback is capped to 5 fps by the
   firmware, can be interrupted by a state change such as PTT/speech, then the
   layered 30 Hz renderer immediately restores the current expression.

This avoids trying to brute-force full-screen video through the shared SPI bus
while still allowing many stored poses and short comic reactions.

## Idle loop / transitions

The firmware schedules an idle beat every 7–17 seconds when there is no active
conversation or server expression. The choices are weighted toward subtle
motion and occasionally use a stronger comic reaction:

- soft smile -> nod -> neutral
- curious -> tilt left/right -> neutral
- proud -> lean -> neutral
- peace -> look around -> neutral
- cozy -> idle moment -> neutral
- mischievous -> shake -> neutral
- nod-yes -> nod -> neutral
- cheerful -> cheer -> neutral

Blink order remains `open -> half -> almost closed -> closed -> almost closed
-> half -> soft lower -> open`. Sleepy expressions use the sleepy-half and
sleepy-closed slots on a slower independent clock. Speaking still uses the
existing fast-open/slow-close viseme smoothing, with smile visemes selected for
happy expressions.

## Source-sheet mapping

The generated references are packed into `assets/source/kizuna_atlas.webp` as
a 4x2 atlas. Each used atlas tile is itself an exact 4x4 cell grid:

| Atlas tile | Runtime use |
|---|---|
| (0,0) expression A | expressions 01–16 |
| (1,0) expression B | expressions 17–32 plus cheerful variant |
| (2,0) eyes | fixed 12 eye slots |
| (3,0) mouth | 8 viseme slots |
| (0,1) head motion | nod/look/tilt/lean/idle clips |
| (1,1) communication gestures | explain/cheer/sleep-pose clips |
| (2,1) full-body poses | visual pose reference for future authored clips |

The runtime recipe references only the first six regions. The full-body region
is intentionally retained in the atlas as production reference without adding
a runtime clip that would consume bandwidth unnecessarily.
