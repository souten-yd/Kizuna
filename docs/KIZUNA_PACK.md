# Kizuna character pack

`kizuna` is the default M5GO companion pack. It is generated from the existing
high-resolution 4x4 animation sources under `assets/source/` plus the recipe in
`assets/characters/kizuna.json`.

The supplied design/reference boards are already represented in this repository
by packer-ready sheets (expression, micro-emotion, eye, mouth, nod, shake, tilt,
head-pose and idle sets). The Kizuna recipe reuses those source files instead of
checking in another multi-megabyte copy of the same character artwork. This also
keeps the current alignment/flood-fill pipeline and its hardware-tested source
quality intact.

## What is in the pack

- 33 named expression intents, including neutral, soft/happy/laughing,
  surprised, embarrassed, shy, pout, curious, relieved, apologetic,
  mischievous, peace, sleepy/yawning, focused, starry, skeptical, startled and
  cozy variants. Some of the subtle intent names currently share the closest
  supplied artwork; their names are independent so a future sheet can replace
  any one of them without changing firmware or protocol.
- 12 eye slots: five gaze directions, progressive blink stages, wide eyes and
  sleepy half/closed states.
- 8 visemes for quiet-to-wide speech plus rounded-O and closed/open smile mouths.
- 10 one-shot gesture clips whose frame lists explicitly return to a stable
  pose: nod, shake, left/right tilt, lean, look-around, idle moment, explain,
  cheer and sleepy pose.

## Build

```bash
python tools/validate_kizuna_sources.py

python tools/pack_assets.py \
  --character assets/characters/kizuna.json \
  --name kizuna \
  --out build/sd

python tools/push_sd.py
```

Or use the convenience wrapper:

```bash
python tools/build_kizuna_pack.py --out build/sd
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

## Animation policy

The 32 GB card is intentionally treated as plentiful storage, but storage size
is not the limiting resource while drawing. The LCD and microSD share one SPI
bus, and a complete 320x240 RGB565 frame is 153,600 bytes. Kizuna therefore
uses two animation lanes:

1. **Continuous 30 Hz lane** — only the 128x44 eye tile and 64x40 mouth tile
   change for blink, gaze and lip sync. These remain responsive during speech.
2. **Comic one-shot lane** — full-screen illustrated gestures play only while
   idle or on explicit physical reactions. Playback is capped to 5 fps by the
   firmware, then the layered 30 Hz renderer immediately restores the current
   expression.

This avoids trying to brute-force full-screen video through the shared SPI bus
while still allowing many stored poses and short comic reactions. A 32 GB card
therefore helps most by letting the pack contain *many* expressions/one-shots;
it does not remove the instantaneous SPI bandwidth limit.

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

Physical/device events can also request a one-shot: shake -> shake clip, pickup
-> tilt, server connection -> nod, sleep request -> sleepy pose. A short nod can
also be chosen after speech completes.

Blink order remains `open -> half -> almost closed -> closed -> almost closed
-> half -> soft lower -> open`. Sleepy expressions use the sleepy-half and
sleepy-closed slots on a slower independent clock. Speaking keeps the existing
fast-open/slow-close viseme smoothing, with smile visemes selected for happy
expressions.

## Source-sheet mapping

| Existing source | Kizuna use |
|---|---|
| `expression_sheet_v2.png` | core expressive faces |
| `emotion_micro.png` | neutral/listening/thinking and subtle emotion variants |
| `eye_direction_set.png` | center/left/right/up/down gaze |
| `eye_blink_set_a.png` | soft-lower and progressive blink stages |
| `eye_blink_set_b.png` | wide and sleepy eye states |
| `mouth_set_a.png` | 8 viseme slots |
| `nod_vertical_set.png` | nod one-shot |
| `head_shake_set.png` | shake one-shot |
| `tilt_lean_set.png` | tilt and lean one-shots |
| `head_pose8_set.png` | look-around / explain-style one-shots |
| `idle_motion_set.png` | idle, cheer and sleep-style one-shots |

These are the same high-resolution source assets the original character pack
already trusts. The Kizuna recipe only changes semantic routing, loop ordering
and runtime behaviour, so future replacement artwork can be dropped in by
editing the recipe rather than rewriting the renderer.
