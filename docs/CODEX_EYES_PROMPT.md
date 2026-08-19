# Codex prompt — Kizuna eye parts only

Self-contained: hand this over as it is. It does not need
[ASSET_BRIEF.md](ASSET_BRIEF.md) pasted underneath, because everything an eye
part depends on is repeated here.

Ask for step 1 first and look at what comes back before asking for step 2.
Requesting all twenty-two at once is what produced twenty-two independent
drawings last time, which is the defect this replaces.

---

## Step 1 — the master

> Draw one image: the open eyes of a character called **Kizuna**, for a
> desktop companion robot.
>
> The character is a semi-realistic anime boy: dark brown wolf-cut hair with
> orange inner streaks, blue-grey eyes, clean line art, soft cel shading. You
> are drawing **only the two eyes** - no face, no hair, no brows, no
> eyelashes belonging to a brow, nothing else.
>
> Technical requirements, all of them mandatory:
>
> - Canvas exactly **1280 x 960 px**, RGBA PNG.
> - The background is **fully transparent**. Not white, not a checkerboard,
>   not a near-white halo around the line art. Alpha 0 everywhere except the
>   eyes themselves.
> - The face's centre line is **x = 640**. Pupil centres sit on **y = 448**
>   and are **192 px apart**, i.e. at x = 544 and x = 736.
> - Each eye is roughly 160 px wide - the head fills most of the canvas, so
>   the eyes are large.
> - Do not crop to the bounding box of the eyes. Do not centre the eyes on
>   their own canvas. The eyes sit where they belong on a full-size canvas
>   that is otherwise empty.
> - No captions, numbers, frames, labels or watermarks anywhere in the image.
>
> Deliver it as `eyes/open.png`.

Check it before continuing: the file must be 1280x960 RGBA, the corners must
be transparent, and opening it over a face layer must need no nudging.

---

## Step 2 — the other twenty-one, as edits of the master

> Here is `eyes/open.png`. Produce the remaining twenty-one eye parts by
> **editing this image**. Every one is the same 1280x960 canvas, the same eye
> shape, the same size, in the same position. Do not redraw them from
> scratch, and do not re-render the character.
>
> **Gaze - four parts.** `look_left`, `look_right`, `look_up`, `look_down`.
> Move **only the iris and its highlight** inside the eye opening. The
> outline, the lid, the lash and the size of the opening are unchanged,
> pixel for pixel, from `open`. Both eyes look the same direction.
>
> **Blink - five parts.** `soft_lower` (lids barely lowered, the resting idle
> pose), `half`, `almost_closed` (a thin sliver of iris), `closed` (fully
> shut, one clean curve), and `open` which you already have. Each stage lowers
> **both lids by the same amount**. The upper lid travels down over an
> otherwise unchanged eye; the outline of the opening is the only thing that
> changes.
>
> **Sleepy - two parts.** `sleepy_half`, `sleepy_closed`. Same rule as blink,
> with the lids heavier and the lower lid slightly raised.
>
> **Expression - eleven parts.** `wide` (attentive, opening larger, iris the
> same size), `surprised` (very wide, iris smaller), `happy_curve` (both eyes
> closed into upward arcs), `smile_soft`, `determined`, `confused`, `sad`,
> `sparkle`, `dizzy`, and the two winks below.
>
> **`wink_left` and `wink_right` are the only two parts where the two eyes
> differ.** One eye is `closed`, the other is `open`, both taken unchanged
> from the parts you already have. Everywhere else the two eyes are in the
> same state.
>
> Deliver twenty-two files in `eyes/`, named exactly as above with `.png`.

---

## Why these rules exist

Say this to Codex if it pushes back, and read it yourself before accepting a
delivery. These are measurements from the pack this one replaces, not
hypotheticals.

The previous eye parts were separate renderings of the same character rather
than layers of one drawing. The packer registers each part against the face by
searching for the translation that lines them up best, and it did that
correctly - but no translation fixes a size difference. What was left over,
after registration:

| Part | Residual offset | On the 320x240 screen |
|---|---|---|
| `open` | (-1, 3) | 1.5 px |
| `look_left` | (-8, 3) | 4.0 px |
| `look_right` | (13, 0) | **6.5 px** |
| `look_up` | (3, 11) | 5.5 px |

An eye is about 40 px wide on that screen, so gaze-right landed a sixth of an
eye away from where the face expected it. Even `open` - the resting gaze,
which should be almost exactly what the face already shows - differed from the
face over **21%** of the eye region. On the device this is debris along the
lower lid that appears and disappears with every blink, and the first thing
anyone notices.

The previous `almost_closed` also closed one eye further than the other.
Because blink fires several times a minute, the character appeared to wink at
the viewer constantly. Nobody had asked for a wink.

Both failures are impossible if the parts are edits of one master, because
there is nothing left to register.

---

## The twelve slots these feed

The firmware addresses twelve eye slots. The remaining ten parts are still
worth having - a recipe can name them for a specific expression - but these
twelve are the ones that animate.

| Firmware slot | Part |
|---|---|
| `open_center` | `open` |
| `open_left` / `open_right` | `look_left` / `look_right` |
| `open_up` / `open_down` | `look_up` / `look_down` |
| `soft_lower` | `soft_lower` |
| `half` | `half` |
| `almost_closed` | `almost_closed` |
| `closed` | `closed` |
| `wide` | `wide` |
| `sleepy_half` / `sleepy_closed` | `sleepy_half` / `sleepy_closed` |

---

## Accepting the delivery

```bash
python tools/validate_assets.py assets/kizuna
```

It fails a part that is not 1280x960 RGBA, that has an opaque background, whose
pupils are off the eye line or the wrong distance apart, or whose two eyes
differ in coverage - the last one being what an unintended wink looks like from
the outside. Fix every FAIL before packing.

Then rebuild and push only the layer that changed:

```bash
python tools/build_kizuna_pack.py --out build/sd
python tools/push_sd.py --only eyes      # about two minutes
```
