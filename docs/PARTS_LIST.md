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

### eye_half is NOT a wink (corrected 2026-08-20)

I read part 02 as a wink at thumbnail size. Enlarged, both lids are lowered
with the iris half hidden - it is the middle of a blink, exactly as specified,
and it now carries `soft_lower`, `half` and `sleepy_half`. The blink runs
open - half - closed - half - open rather than snapping between two frames.
The paragraph below is kept because the finding it records still holds.

Part 02 was thought to be one eye shut rather than both lids lowered, which
would have meant it could not serve as the middle of a blink. Cross-fading open and closed was tried in its
place and the iris ghosts through as a translucent disc - a fade is not a lid
coming down. The blink is two stages for now.

**Part 09 - both eyes half-closed.** Same rules as 01-08: change only the
eyelids, leave the eyes' position, the hair, the jacket and the framing alone.
When it arrives, `soft_lower`, `half` and `sleepy_half` in
`tools/build_variant_pack.py` take it and the blink softens with no code change.
The wink has no firmware slot yet and is held aside rather than misused.

## 2026-08-20 evening - the featureless base

A picture of the character with no eyes and no mouth (`blank_face2.png`) is now
the base frame. Two things follow from it that did not hold before.

The rectangles can be measured properly. Previously the eye rectangle came from
master vs eye_closed and clipped 14 px off the top of the open eyes, because a
closed lid does not reach as high as an open one. Against a base with no eyes at
all the difference is the eye itself, in every state, so the union of the states
is the rectangle. Hair is excluded by requiring the base to be skin at that
pixel - the two pictures are separate renders whose hair strands differ
everywhere, and without that test the rectangle grows to the whole head.

| feature | rect | bytes/blit | ceiling |
|---|---|---|---|
| eyes  | (113,70) 108x49 | 10584 | 80 fps |
| mouth | (140,119) 62x33 |  4092 | 207 fps |

**The two rectangles must not overlap.** DisplayTask redraws the eyes and the
mouth independently between body frames, so an overlapping eye rectangle lets a
blink repaint the top of a mouth that is mid-word. The packer now refuses to
build such a pair rather than leaving it to be noticed on hardware.

### Registration is required

These are re-renders, not edits, and each came back at its own offset: the base
at +2.0 screen px, eye_half at +2.4, the mouths and the gaze pairs at -1.0, and
only eye_closed at 0. A mouth cut from one and drawn onto another therefore
landed 3 px off the face. `tools/register.py` measures the offset on hair and
clothing - never on the face, which is what legitimately differs - and corrects
it on the full canvas so the residual survives the downscale as a fraction of a
screen pixel.

### Feather only the edges that sit on hair

A hard rectangle edge running through hair reads as a step twice the size of
anything the drawing does on its own (59 against 30). Fading the last 3 px into
the base removes it. But fading every edge ate the lower lash line, because the
eye rectangle's bottom edge runs across the cheek, where the two renders already
agree and there was nothing to hide. The eyes fade on three sides and the mouth
on three, each leaving the shared boundary hard.

## Part 09 - the wink - NOT NEEDED

A wink needs no picture of its own, and the reason is worth keeping.

The two eyes occupy separate columns of the eye rectangle - x 121..145 and
x 169..197 - and between them, x 145..165, every eye picture agrees to within
4 levels out of 255. They agree because eye_closed is a true edit of master
rather than a re-render: measured offset 0, residual 1.10 where the re-rendered
parts sit at 5. So the wink is master's left half joined to eye_closed's right
half at x=157, and the join shows nothing, because there was never anything
there to hide.

This generalises past the wink. Any eye may hold any state independently of the
other, from the pictures already delivered - one eye half-lidded while the other
watches, a slow one-sided close, a wink held while the mouth talks. Five eye
pictures give twenty-five pairings for the cost of the five.

`splice()` in tools/build_variant_pack.py does the join; a slot names either one
picture or a pair. The paragraphs below are what I wrote when I thought a
drawing was needed, kept because the framing rules in it apply to any new part.

### The request that turned out to be unnecessary Everything else for it exists: `include/AssetFormat.hpp`
has `kEyeWink`, `include/AppTypes.hpp` has `EyeFrame::Wink`, DisplayTask maps
between them, and CharacterDirector spends a wink on the Playful and
Mischievous expressions and holds it 360 ms - long enough to read as deliberate
rather than as a blink that failed on one side. Until the picture arrives the
slot holds the open eyes, so the expressions simply do not wink.

Request to the generator, same rules as parts 01-08:

> Take the attached picture of the character. Change one thing: close her left
> eye (the one on the right side of the picture) as if winking - the lid fully
> down with the lashes drawn as a curved line, the way part 01 draws a closed
> eye. Leave her other eye exactly as it is, fully open.
>
> Everything else must be identical to the original: the same pose, the same
> framing and crop, the same hair with every strand in the same place, the same
> jacket, collar and pendant, the same background, the same image size. Do not
> redraw, do not re-render, do not shift or rescale the character. Do not
> change the mouth or the eyebrows.
>
> Return the full picture, not a crop of the eye.

A redraw is survivable - the packer keeps only the eye rectangle and registers
the picture against the master first - but a shifted or rescaled character is
not, and neither is a changed crop.
