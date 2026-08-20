# Codex prompt — seven edits of one picture

The smallest commission this project has needed, and the one that finally
matches how the device works.

## Why it is only seven files

The device draws the face once and then repaints **only the rectangle that
changed** when the eyes or the mouth move. That rectangle is not specified
anywhere - it is worked out by comparing the variant with the picture it came
from. Which means the whole thing rests on one property:

**everything except the feature must be identical.**

Not similar. Identical. The existing asset sheets were checked against this and
fail it comprehensively: in `eye_blink_set_a`, a *blink* differs from its
neighbouring frame across the hair, the jacket and the shading, and the body
differs 126% as much as the head does. Aligning them helps by a quarter and no
more, because the frames are not misaligned - they are separate drawings.

So these seven cannot be generated afresh. They have to be edits.

## The request

Attach `base.png`. Then, one file at a time:

> Here is a picture of a character. Return **the same picture** with one change:
> **the eyes are closed**.
>
> Everything else must be identical, pixel for pixel - the hair, every strand
> of it, the jacket, the collar, the choker, the shading, the background, the
> framing, the head position. Do not redraw the character. Do not re-render.
> Edit this image and change nothing but the eyes.
>
> Return it at the same size you received it.

Then the same sentence with each of these in place of "the eyes are closed":

| File | The one change |
|---|---|
| `eye_half.png` | the eyes are half closed, lids halfway down |
| `eye_closed.png` | the eyes are fully closed |
| `eye_left.png` | the eyes look to the character's left - **the irises move, the eye shape does not** |
| `eye_right.png` | the eyes look to the character's right - the irises move, the eye shape does not |
| `mouth_small.png` | the mouth is slightly open, as at the start of a word |
| `mouth_medium.png` | the mouth is open about halfway |
| `mouth_wide.png` | the mouth is wide open |

Seven files. One instruction each.

## Checking them

```bash
python tools/diff_variants.py <the folder with base.png and the seven>
```

A file that was **edited** gives a small, densely filled rectangle around the
feature:

```
eye_closed.png        424,296 408x128     102x32     6528   78%
```

A file that was **redrawn** gives a rectangle spanning the canvas, and the tool
says so:

```
eye_closed.png         48,48 1080x912    270x228   123120   60%  <- spans the canvas; the whole picture was redrawn
```

That check is not theoretical. Run against the eight sheets already in this
repository it rejects every one of them, correctly, which is how we know the
existing artwork cannot serve as variants and why these seven are being asked
for at all.

The size coming back different is fine - it is scaled to match before
comparing. A different *shape* is not, and stops there.

## What good looks like

- Each rectangle is a few hundred canvas pixels across, not a thousand.
- Fill is above about 50%: a tight box that is mostly changed, rather than a
  large box with a few scattered differences in it.
- The mouth variants' rectangles sit below the eye variants' rectangles, and
  neither reaches the hair.

If a file fails, ask again with the same sentence rather than a longer one. The
instruction is not ambiguous; the failure is that it was regenerated instead of
edited, and more words do not fix that.
