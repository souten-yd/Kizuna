# Asset production brief — Kizuna companion

This is the specification to hand to whoever (or whatever) generates the
artwork. It exists because every alignment problem this project has had came
from the same source: parts delivered as loose crops on white backgrounds,
which then had to be cut out and positioned by detection. Detection guesses.
Registration does not.

**The single most important rule: every image is drawn on the same canvas, at
the same scale, in the position it occupies in the final picture, with a
transparent background.** Assembling a face is then `base + brows + eyes +
mouth + fx` with no offsets and no scaling. Get this right and everything
else is easy.

---

## 1. Canvas and registration

| Property | Value |
|---|---|
| Master canvas | **1280 x 960 px**, RGBA PNG, transparent background |
| Device screen | 320 x 240 (the master is 4x, downscaled at pack time) |
| Face centre line | **x = 640** |
| Eye line (pupil centres) | **y = 448** |
| Interocular distance | **192 px** between pupil centres |
| Mouth centre | **y = 576** |
| Brow line | **y = 380** |
| Top of hair | y >= 40 (never clipped) |
| Chin | y ~ 700 |
| Shoulders / body | continue to the bottom edge, cropped by it |
| Character width | ~760 px at the widest (hair), centred on x = 640 |

Every part file is a full 1280 x 960 canvas that is transparent except for
that part. An eye file contains only the two eyes, sitting exactly where they
belong. A mouth file contains only the mouth. Do **not** crop parts to their
own bounding box, and do **not** centre them on their own canvas.

Deviating from the eye line or the interocular distance by even a few pixels
is what produces a doubled or floating eye on the device, so treat those two
numbers as fixed.

### Backgrounds

**Never bake in a background.** Not white, not a gradient, not a room. The
device composites its own backdrop behind the character - and the user wants to
change that backdrop later, to a room or a photo or a flat colour, without
regenerating a single character asset. A baked background makes that
impossible.

If backdrop artwork is wanted, deliver it separately as opaque 1280 x 960
images under `backgrounds/`.

---

## 2. File layout

One PNG per part. No contact sheets, no captions inside the image, no card
frames, no numbering baked into the artwork. Sheets have to be split by
detecting a grid, and the captions get cut out along with the artwork.

```
kizuna/
  base/          face with no eyes, no mouth, no brows - hair, skin, body
    front.png
    tilt_left.png        tilt_right.png
    turn_left_15.png     turn_right_15.png
    turn_left_30.png     turn_right_30.png
    look_up.png          look_down.png
    nod_down.png         nod_up.png
    idle.png
  eyes/          eyes only, on the eye line
  mouths/        mouth only, on the mouth line
  brows/         brows only, on the brow line
  fx/            blush, sweat, sparkles, symbols - each in its final position
  body/          full-body poses, separate canvas rules (section 6)
  character.json the metadata file (section 5)
```

Names are lowercase with underscores and are what the metadata refers to.

---

## 3. Part list

### 3.1 Base faces — 12

Blank faces: hair, ears, skin, neck, clothing. **No eyes, no eyebrows, no
mouth, no blush.** A faint nose is fine.

`front`, `idle`, `tilt_left`, `tilt_right`, `turn_left_15`, `turn_right_15`,
`turn_left_30`, `turn_right_30`, `look_up`, `look_down`, `nod_down`, `nod_up`

Head angle changes the eye and mouth positions in the real drawing. Because
every layer must stay registered, deliver **matching eye/mouth/brow parts for
each non-frontal angle** - see 3.6. If that is too much work, deliver only
`front` and `idle` at full quality and treat the angled bases as a later
addition; the companion is expressive without them.

### 3.2 Eyes — 22

Blink needs at least five stages or it reads as a light switch:

| Name | Notes |
|---|---|
| `open` | the resting eye |
| `soft_lower` | lids barely lowered - the idle resting pose |
| `half` | halfway |
| `almost_closed` | a thin sliver of iris |
| `closed` | fully shut, a clean curve |

Gaze, four directions, iris shifted inside the same eye shape:
`look_left`, `look_right`, `look_up`, `look_down`

Expression eyes:
`wide` (attentive), `surprised` (very wide, small iris), `happy_curve`
(closed upward arcs), `smile_soft`, `wink_left`, `wink_right`, `sleepy_half`,
`sleepy_closed`, `determined`, `confused`, `sad`, `sparkle`, `dizzy`

### 3.3 Mouths — 20

Visemes first - these drive lip sync and must form a smooth size ramp:

`rest` (closed), `tiny`, `small`, `medium`, `wide`,
`viseme_a`, `viseme_i`, `viseme_u`, `viseme_e`, `viseme_o`

Expression mouths:
`smile_closed`, `smile_open`, `grin`, `teeth_smile`, `laugh_wide`,
`pout`, `sad`, `mmm`, `yawn`, `shout`

### 3.4 Brows — 8

`neutral`, `happy`, `sad`, `angry`, `surprised`, `focused`, `worried`,
`relaxed`

### 3.5 FX overlays — 12

Each already positioned where it belongs on the face or beside the head:

`blush_light`, `blush_strong`, `sweat_drop`, `tear`, `question`,
`exclamation`, `anger_mark`, `sparkle`, `emphasis_lines`, `zzz`,
`music_note`, `heart`

### 3.6 Angled variants (optional, high value)

For each non-frontal base face, the same eye / mouth / brow parts redrawn to
match that head angle, in `eyes/<angle>/`, `mouths/<angle>/`, `brows/<angle>/`.
Start with `tilt_left`, `tilt_right` and `look_down`; those three cover most
idle motion.

---

## 4. Motion, and the one hard constraint

The device's LCD and its SD card share a single SPI bus, so artwork crosses it
twice - read from the card, written to the panel. About **850 KB/s** fits.
That produces a hard split, and it should shape how sequences are designed:

| What changes | Cost per frame | Achievable rate |
|---|---|---|
| Eyes only | 11 KB | 30 fps |
| Mouth only | 5 KB | 30 fps |
| Eyes + mouth | 16 KB | 30 fps |
| Whole head or body | 150 KB | ~7 fps |

So: **blinks, lip sync and gaze can be as smooth as you like. Head movement
cannot.** Design idle loops that are mostly eyes and mouth, punctuated
occasionally by a head move - which is also what a person actually does when
sitting still. A nod is fine at 7 fps for half a second. A continuous head
sway at 7 fps is not fine; it looks like a slideshow.

Sequences should therefore be written as:

- **micro loops** - eyes and mouth only, running continuously,
- **gestures** - short head or body moves, triggered by events, played once.

---

## 5. Metadata — `character.json`

Artwork without this is a pile of PNGs. This file is what makes it a
character: what each expression *means*, what it is built from, how it loops,
and what plays when moving between two of them.

```jsonc
{
  "character": "kizuna",
  "version": 1,
  "canvas": { "w": 1280, "h": 960 },
  "registration": {
    "face_center_x": 640,
    "eye_line_y": 448,
    "interocular": 192,
    "mouth_y": 576,
    "brow_y": 380
  },

  // What the device shows, and why. `meaning` is documentation for humans and
  // for the language model driving the companion - it is how the server knows
  // which expression to ask for.
  "expressions": {
    "neutral": {
      "meaning": "resting, attentive but not engaged",
      "base": "front", "brows": "neutral",
      "eyes": "open", "mouth": "rest",
      "idle": "idle_breathe",
      "accent": [44, 78, 116]
    },
    "happy": {
      "meaning": "the user said something good, or a task succeeded",
      "base": "front", "brows": "happy",
      "eyes": "happy_curve", "mouth": "smile_open",
      "fx": ["blush_light"],
      "idle": "idle_breathe",
      "accent": [150, 96, 30]
    },
    "shy": {
      "meaning": "praised, or asked something personal",
      "base": "tilt_right", "brows": "worried",
      "eyes": "look_down", "mouth": "smile_closed",
      "fx": ["blush_strong", "sweat_drop"],
      "idle": "idle_shy",
      "accent": [170, 90, 110]
    },
    "sleepy": {
      "meaning": "idle for a long time, or late at night",
      "base": "look_down", "brows": "relaxed",
      "eyes": "sleepy_half", "mouth": "rest",
      "fx": ["zzz"],
      "idle": "idle_doze",
      "accent": [36, 44, 92]
    }
    // ... about thirty of these, see 5.1
  },

  // Micro loops: eyes and mouth only, cheap enough to run continuously.
  "sequences": {
    "blink": {
      "layer": "eyes", "mode": "once",
      "frames": [
        ["soft_lower", 30], ["half", 30], ["almost_closed", 30],
        ["closed", 60], ["almost_closed", 30], ["half", 30],
        ["soft_lower", 30], ["open", 0]
      ]
    },
    "idle_breathe": {
      "mode": "loop", "interrupts": ["blink"],
      "blink_every_ms": [2600, 6400],
      "gaze_wander_ms": [1400, 5200]
    },
    "idle_doze": {
      "layer": "eyes", "mode": "pingpong",
      "frames": [["sleepy_half", 1400], ["sleepy_closed", 2200]]
    },
    "nod": {
      "layer": "base", "mode": "once", "fps": 7,
      "frames": ["front", "nod_down", "nod_down", "nod_up", "front"]
    },
    "shake_head": {
      "layer": "base", "mode": "once", "fps": 7,
      "frames": ["front", "turn_left_15", "turn_right_15",
                 "turn_left_15", "front"]
    }
  },

  // What to play when moving from one expression to another. Optional; a
  // missing entry means a straight cut, which is fine for most pairs.
  "transitions": [
    { "from": "*",        "to": "happy",    "play": "nod" },
    { "from": "*",        "to": "confused", "play": "tilt_question" },
    { "from": "sleepy",   "to": "*",        "play": "wake_stretch" },
    { "from": "thinking", "to": "speaking", "play": "nod" }
  ],

  // Loudness bucket -> mouth part. The device measures the audio envelope and
  // picks a bucket 30 times a second.
  "visemes": {
    "levels": ["rest", "tiny", "small", "medium", "wide"],
    "phonemes": {
      "A": "viseme_a", "I": "viseme_i", "U": "viseme_u",
      "E": "viseme_e", "O": "viseme_o", "M": "mmm"
    },
    "smiling_variants": { "rest": "smile_closed", "wide": "smile_open" }
  }
}
```

### 5.1 The thirty expressions

Thirty expressions do **not** mean thirty drawings. They are combinations of
the parts above, and that is the whole point of a layered rig - the artwork
budget goes into parts, and expressions are configuration.

Deliver these, defined in `character.json`:

| # | Name | Meaning |
|---|---|---|
| 1 | `neutral` | resting |
| 2 | `idle_soft` | resting, lids slightly lowered |
| 3 | `attentive` | listening to the user |
| 4 | `listening` | actively recording |
| 5 | `thinking` | working on an answer |
| 6 | `speaking` | talking |
| 7 | `happy` | pleased |
| 8 | `delighted` | very pleased |
| 9 | `excited` | something great happened |
| 10 | `proud` | finished something well |
| 11 | `playful` | teasing, winking |
| 12 | `cheeky` | mischievous |
| 13 | `shy` | praised or embarrassed |
| 14 | `bashful` | strongly embarrassed, looking away |
| 15 | `curious` | head tilted, interested |
| 16 | `confused` | did not understand |
| 17 | `surprised` | startled |
| 18 | `worried` | something looks wrong |
| 19 | `sad` | bad news |
| 20 | `apologetic` | it made a mistake |
| 21 | `determined` | starting a hard task |
| 22 | `focused` | concentrating |
| 23 | `relieved` | the problem is fixed |
| 24 | `sleepy` | drowsy |
| 25 | `dozing` | eyes closed, asleep |
| 26 | `waking` | just woken |
| 27 | `bored` | nothing has happened for a while |
| 28 | `impressed` | the user did something clever |
| 29 | `affectionate` | warm, fond |
| 30 | `error` | something is broken |

For each, fill in `meaning`, the layer choices, any `fx`, an `idle` sequence
and an `accent` colour. `meaning` matters as much as the artwork: it is what
lets the language model pick an expression that fits what it just said.

---

## 6. Body poses

Separate canvas rules, because the head is no longer the subject:

- 1280 x 960, RGBA, transparent, full body inside the frame with 40 px margin.
- Faces on body poses may be drawn complete - they are used as whole frames,
  not composited.
- Suggested set: `stand`, `wave`, `point`, `thinking`, `typing`, `sitting`,
  `jump`, `thumbs_up`, `hands_on_hips`, `holding_device`, `sleeping`,
  `stretching`.

---

## 7. Checklist before delivery

- [ ] Every file is RGBA with a genuinely transparent background - no white
      rectangle, no near-white halo around the silhouette.
- [ ] Every file is exactly 1280 x 960.
- [ ] Parts sit in their final position on the canvas; opening two part files
      as layers in any editor produces a correct face with no nudging.
- [ ] Pupil centres are on y = 448 and 192 px apart in every eye file.
- [ ] No captions, numbers, card frames or logos anywhere in an image.
- [ ] Base faces have no eyes, no brows and no mouth.
- [ ] `character.json` names only files that exist, and every file is named
      by it.
- [ ] Blink has at least five stages and the viseme ramp is monotonic.

---

## 8. Verifying a delivery

```bash
python tools/validate_assets.py assets/kizuna
```

It checks canvas size and mode, that backgrounds are genuinely transparent,
that eye files sit on the eye line at the right separation, that base faces
have no eyes drawn on them, and that `character.json` and the files on disk
agree in both directions. Fix everything it reports as FAIL before packing.
