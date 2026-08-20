# Parts list

What has to exist for the face to animate, in the order it should be asked for.
Every numbered file except 01 is **the same picture as 01 with one thing
changed** - see [CODEX_MASTER_PROMPT.md](CODEX_MASTER_PROMPT.md).

## Now: the front face

| # | File | The one change | Rectangle | Status |
|---|---|---|---|---|
| 01 | `master.png` | — the resting pose: front, eyes open, mouth closed | — | **done** |
| 02 | `eye_closed.png` | the eyes are fully closed | eyes | **done** |
| 03 | `eye_half.png` | the eyes are half closed, lids halfway down | eyes | **done** |
| 04 | `eye_left.png` | the eyes look to the character's left — the irises move, the eye shape does not | eyes | pending |
| 05 | `eye_right.png` | the eyes look to the character's right — same rule | eyes | pending |
| 06 | `mouth_small.png` | the mouth is slightly open, as at the start of a word | mouth | pending |
| 07 | `mouth_medium.png` | the mouth is open about halfway | mouth | pending |
| 08 | `mouth_wide.png` | the mouth is wide open | mouth | pending |

Two rectangles, each taken from the extreme of its feature, because the
extreme bounds everything in between:

```bash
python tools/diff_variants.py assets/kizuna/variants \
    --base master.png --fixed-from eye_closed.png     # 02-05
python tools/diff_variants.py assets/kizuna/variants \
    --base master.png --fixed-from mouth_wide.png     # 06-08
```

Measured on 02: **117 x 111 on screen, 26 KB, 33 fps.** Above the 30 Hz the
display loop runs at, so blink and lip sync are not rationed.

Anything a variant changes outside its rectangle is discarded rather than
drawn. That is not a compromise - it is what makes a generator's incidental
redrawing harmless. 03 came back with the head moved slightly; the movement
falls outside the eye rectangle and never reaches the screen.

## Next: expressions, from the same eight

An expression is a combination, not a drawing. With four eye states and four
mouth states there are sixteen faces before anything else is commissioned, and
which combination means what is written in `character.json` rather than in the
artwork:

| Expression | Eyes | Mouth |
|---|---|---|
| neutral | 01 | 01 |
| listening | 01 | 01 + a head angle |
| speaking | 01 | 06 / 07 / 08 in sequence |
| happy | 03 (softened) | 07 |
| sleepy | 03 | 01 |
| surprised | 01 | 08 |

Brows would double this again and are worth adding only if the sixteen turn
out not to be enough.

## Later, and cheap: head angles

Already drawn. `head_pose8_set.png` holds sixteen heads - front, turned left
and right at two depths, chin up, chin down - and they render cleanly at
320x240 because a whole cell has no seam to get wrong.

They are used as **cuts**, not as animation: the head changes angle in one
repaint and the eyes lead the way there. A drawn turn would need seven heads
per angle for motion the bus cannot deliver smoothly anyway.

There is one limit: those sheets are 1122 x 1402 for a 4x4 grid, so a cell is
about 200 x 270 - enough for a 320 x 184 picture and no more. Variants cannot
be made from them, which is why 01 was drawn fresh.

## Later, and not yet specified: poses

A body layer, so the hand stays at the ear while the face keeps blinking. See
[POSE_AND_ANGLE_SPEC.md](POSE_AND_ANGLE_SPEC.md); the framing question there
is settled but the artwork has not been asked for.
