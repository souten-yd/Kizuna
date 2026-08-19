# Kizuna character pack

`kizuna` is the default M5GO companion artwork pack. A fresh clone contains all
source artwork needed to build it: the recipe lives in
`assets/characters/kizuna.json` and it reuses the repository's high-resolution,
packer-ready 4x4 source sheets under `assets/source/`.

This is deliberate. The pack can be cloned, built and copied to an SD card
without downloading a second binary asset bundle. Future dedicated Kizuna art
can replace individual sheet mappings in the recipe without changing the
firmware or wire protocol.

## What is in the pack

- 33 named expression intents, including neutral, soft/happy/laughing,
  surprised, embarrassed, shy, pout, curious, relieved, apologetic,
  mischievous, peace, sleepy/yawning, focused, starry, skeptical, startled and
  cozy variants. Some subtle intent names currently share the closest supplied
  artwork; the semantic names remain independent so dedicated art can replace
  them later without firmware changes.
- 12 eye slots: five gaze directions, progressive blink stages, wide eyes and
  sleepy half/closed states.
- 8 visemes for quiet-to-wide speech plus rounded-O and closed/open smile mouths.
- 10 one-shot gesture clips whose frame lists explicitly return to a stable
  pose: nod, shake, left/right tilt, lean, look-around, idle moment, explain,
  cheer and sleepy pose.

## Clone -> build -> deploy

```bash
git clone https://github.com/souten-yd/Kizuna.git
cd Kizuna

# firmware
pio run -e m5go -t upload

# validate and build the default Kizuna artwork pack
python tools/validate_kizuna_sources.py
python tools/build_kizuna_pack.py --out build/sd

# copy incrementally to the M5GO microSD over USB
python tools/push_sd.py
```

The generated pack is:

```text
build/sd/companion/packs/kizuna/
  manifest.json
  base/*.m5a
  eyes/*.m5a
  mouth/*.m5a
  clips/*.m5a
```

New installations default to `pack = kizuna`. Existing installations that
already saved a different pack name in NVS keep that explicit choice; switch
packs from the web pack manager or set `"pack": "kizuna"` in
`/companion/config/device.json`.

## Animation policy

A 32 GB card gives the character room for many expressions and short comic
clips, but card capacity is not the real-time bottleneck. The LCD and microSD
share one SPI bus and one complete 320x240 RGB565 frame is 153,600 bytes.
Kizuna therefore uses two animation lanes:

1. **Continuous 30 Hz lane** - only the 128x44 eye tile and 64x40 mouth tile
   change for gaze, blink and lip sync. These remain responsive during speech.
2. **Comic one-shot lane** - full-screen illustrated gestures play at 3-5 fps,
   then the layered renderer immediately restores the current expression.

Full-screen gestures are interruptible. A real interaction such as PTT,
speaking/thinking state change, sleep/wake or a new gesture request takes
priority, so an idle animation cannot make the companion feel unresponsive.
At the 5 fps cap the interruption latency is bounded to roughly one gesture
frame (about 200 ms) plus the normal renderer work.

## Idle loop / transitions

When there is no active conversation or server expression, the firmware can
schedule small personality beats rather than repeating one mechanical loop.
The intended connections are:

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
also be selected after speech completes.

Blink order remains `open -> half -> almost closed -> closed -> almost closed
-> half -> soft lower -> open`. Sleepy expressions use the sleepy-half and
sleepy-closed slots on a slower independent clock. Speaking keeps fast-open /
slow-close viseme smoothing, with smile visemes selected for happy expressions.

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

The asset recipe is data rather than renderer code. That is the intended path
for replacing backgrounds, adding dedicated expressions, or swapping in a
future fully registered Kizuna art delivery while keeping the device runtime
small and deterministic.
