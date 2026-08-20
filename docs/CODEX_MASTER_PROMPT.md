# Codex prompt — the master picture, and seven edits of it

Supersedes [CODEX_VARIANT_PROMPT.md](CODEX_VARIANT_PROMPT.md), which tried to
build the variants on top of the artwork already in this repository. That does
not work, and the reason is worth writing down before the requests.

## Why the existing sheets cannot be the master

The asset sheets are **1122 x 1402 for a 4x4 grid**, so one cell is about
**200 x 270 pixels**. Handing that to a generator as a 1280 x 960 canvas means
handing it a picture enlarged six times - a picture whose fine detail does not
exist. Asked to *edit* it, the only thing an image model can do is invent the
detail back, which is a redraw wearing the word "edit".

That is exactly what came back when it was tried. Against the base it was
given, the returned file matched in the flat areas - mean difference 6.7 of
255 - and differed across every strand of hair and every edge. The rectangle
holding 80% of that difference was 170 x 157 on screen: 53 KB, 16 fps. The
thing the whole scheme exists to avoid.

So the master has to be born at the generator's own resolution. Then an edit of
it is an edit of something real.

## Stage 1 — the master

Ask for this alone. Everything else is an edit of it, and an error here is
inherited by all seven.

> Draw a character called **Kizuna**, the companion for an M5Stack device.
>
> He is a semi-realistic anime boy: dark brown wolf-cut hair with orange inner
> streaks, blue-grey eyes, an orange-lined black hoodie jacket over a white
> tee, a black choker with a small charm, and a white "M5" hair clip above his
> right eye. Clean line art, soft cel shading.
>
> **Front facing, looking at the viewer. Eyes open, neutral. Mouth closed,
> neutral.** This is the resting pose everything else is drawn from, so it
> should be calm rather than characterful.
>
> Composition:
>
> - Head and upper torso, framed so that a hand raised to the ear or the chin
>   would be inside the picture. Not a close-up of the face; not a full body.
> - **The whole head is inside the frame**, with clear space above the hair.
>   No strand touching the top edge.
> - **Nothing important in the bottom quarter.** The device draws a text panel
>   over the bottom 23% of the picture, so the chest and shoulders can go
>   there but nothing that needs to be seen.
> - 4:3, as large as you like. The pixel size does not matter; what matters is
>   that every later file is the same size as this one.
> - Background **alpha 0** - transparent, not white and not a faint wash. The
>   device draws its own backdrop.
>
> No captions, numbers, frames, labels or watermarks anywhere in the image.

Check it before going on:

```bash
python tools/make_part_template.py base      # what it will look like on the device
```

## Stage 2 — seven edits

Attach the master. One file at a time, one sentence each.

> Here is a picture of a character. Return **the same picture** with one
> change: **the eyes are closed**.
>
> Everything else must be identical, pixel for pixel - the hair, every strand
> of it, the jacket, the collar, the choker, the shading, the background, the
> framing, the head position. Do not redraw the character. Do not re-render.
> Edit this image and change nothing but the eyes.
>
> Return it at the same size you received it.

Then the same sentence with each of these in place of the change:

| File | The one change |
|---|---|
| `eye_half.png` | the eyes are half closed, lids halfway down |
| `eye_closed.png` | the eyes are fully closed |
| `eye_left.png` | the eyes look to the character's left - **the irises move, the eye shape does not** |
| `eye_right.png` | the eyes look to the character's right - the irises move, the eye shape does not |
| `mouth_small.png` | the mouth is slightly open, as at the start of a word |
| `mouth_medium.png` | the mouth is open about halfway |
| `mouth_wide.png` | the mouth is wide open |

## Accepting them

```bash
python tools/diff_variants.py assets/kizuna/variants
```

An **edit** gives a small, densely filled rectangle around the feature:

```
eye_closed.png        424,296 408x128     102x32     6528   78%
```

A **redraw** gives one spanning the canvas, and the tool says so.

The size coming back different is fine - it is scaled to match before
comparing. A different aspect is not.

## If stage 2 still comes back redrawn

It is possible that this generator cannot edit at all, only regenerate. That
would be worth knowing plainly rather than working around, and the test above
establishes it in seconds.

If so, the fallback is not more prompting. It is to accept whole frames -
which is what the device does today at 11.7 fps - and spend the effort on the
things that are not limited by the artwork: the microphone, the voice loop,
and the parts of the display budget that are already free.
