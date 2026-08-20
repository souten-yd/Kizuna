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
| 04 | `eye_left.png` | the eyes look to the character's left — the irises move, the eye shape does not | eyes | **done** |
| 05 | `eye_right.png` | the eyes look to the character's right — same rule | eyes | **done** |
| 06 | `mouth_small.png` | the mouth is slightly open, as at the start of a word | mouth | redrawn — ask again |
| 07 | `mouth_medium.png` | the mouth is open about halfway | mouth | redrawn — ask again |
| 08 | `mouth_wide.png` | the mouth is wide open | mouth | redrawn — ask again |

The four eye variants came back as genuine edits: the difference sits on the
eyes and the hair and jacket are untouched. **125 x 135 on screen, 25 fps.**

The three mouth variants did not. Their difference covers the head outline,
the jacket and the shoulders as well as the mouth, which puts the rectangle at
204 x 188 and the rate at 11 fps. Since the same generator, the same master
and the same sentence produced clean edits for the eyes, this is worth asking
for again rather than accepting - and the re-request should name the eyes
among the things that must not change, since those are the part it has already
shown it can leave alone.

One thing that does *not* help: taking the rectangle from `mouth_small` against
`mouth_wide` rather than against the master. Each variant carries its own
regeneration drift, so comparing two of them compounds the drift instead of
cancelling it - measured, 204 x 188 becomes 223 x 205.

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

## 2026-08-20 - what the delivered parts actually cost

The verdict on 06-08 recorded above ("redrawn, re-request") was wrong, and the
mistake is worth keeping because it is easy to repeat. A variant that differs
from the master across the whole head is not thereby unusable. Only the feature
rectangle is ever drawn, so a redraw outside it costs nothing. What the redraw
does damage is the *automatic* rectangle, which grows to cover every difference
it can find and so reported the mouth as 204x188 - the whole face.

Telling the search where to look fixes it. `diff_variants.py --region X0 Y0 X1 Y1`
confines the search to a box in screen pixels, and the mouth came back at 97x60.

Final rectangles, both verified by compositing every state onto the master and
looking at the result rather than at a seam score:

| feature | rect (screen) | bytes/blit | ceiling |
|---|---|---|---|
| eyes  | (122,90) 72x23  |  3312 | 255 fps |
| mouth | (117,115) 97x60 | 11640 |  73 fps |

Against a bus carrying ~850 KB/s, neither is a constraint any more; the frame
rate of the face is now set by whatever else is on the bus, not by the face.

The seam score stayed high (11-17) for every mouth state and for eye_half /
eye_left / eye_right, and every one of them composites cleanly. The score
measures how much the variant differs just outside its rectangle, which for a
redrawn variant is large by construction and says nothing about whether the
join shows. Treat it as a hint about which parts to look at, never as a gate.

### eye_half is a wink

Part 02 arrived as one eye shut rather than both lids lowered, so it cannot
serve as the middle of a blink. Cross-fading open and closed was tried in its
place and the iris ghosts through as a translucent disc - a fade is not a lid
coming down. The blink is two stages for now.

**Part 09 - both eyes half-closed.** Same rules as 01-08: change only the
eyelids, leave the eyes' position, the hair, the jacket and the framing alone.
When it arrives, `soft_lower`, `half` and `sleepy_half` in
`tools/build_variant_pack.py` take it and the blink softens with no code change.
The wink has no firmware slot yet and is held aside rather than misused.
