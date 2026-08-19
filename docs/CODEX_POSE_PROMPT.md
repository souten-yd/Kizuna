# Codex prompts — poses and head angles

Self-contained, like the eye and base prompts. Read
[POSE_AND_ANGLE_SPEC.md](POSE_AND_ANGLE_SPEC.md) first; it explains why the
layers are split the way they are, and the split is the part that matters.

**Hand over the guide image with every request.** Generate it with:

```bash
python tools/make_part_template.py body                    # for poses
python tools/make_part_template.py base --angle turn_right_15
python tools/make_part_template.py eyes --angle turn_right_15
```

It is built from what has already been delivered, so it shows the artwork the
new part has to sit with rather than a second description of it.

Three deliveries in a row came back on the wrong canvas with the features up to
2.5x out, against a spec that stated every number. Prose coordinates do not
survive; a picture does.

---

## A. Poses — seven bodies

Ask for `idle` alone first. Everything else is an edit of it, and if `idle` is
wrong every pose inherits the error.

### A1 — the neutral body

> Draw the body of a character called **Kizuna**, for a desktop companion
> robot, from the neck down.
>
> He wears an orange-lined black hoodie jacket over a white tee and a black
> choker with a small "M5" charm. Semi-realistic anime, clean line art, soft
> cel shading. Match the attached guide - the same jacket, the same collar, the
> same shoulders, in the same place.
>
> **Draw nothing above the neck.** No head, no hair, no chin. The head is a
> separate layer composited on top of this one, and anything drawn here shows
> through it.
>
> - Canvas exactly **1280 x 960 px**, RGBA PNG. The same size as the guide.
> - Background **alpha 0** - not white, not a faint wash.
> - The shoulders and collar sit exactly where the guide shows them. This file
>   is the reference every other pose is edited from, so its placement is the
>   placement.
> - No captions, numbers, frames or watermarks.
>
> Deliver **one file**, `body/idle.png`.

### A2 — six poses, as edits

> Here is `body/idle.png`. Produce six more poses by **editing this image**.
> The collar, the jacket and the shoulder line stay where they are unless the
> pose moves that shoulder; everything you add is an arm and a hand coming into
> frame.
>
> **Keep the hands at or above shoulder height.** The frame is head and
> shoulders - there is no room below. A hand on a hip is off-screen; a hand at
> the ear is not.
>
> | File | The state it belongs to | The body |
> |---|---|---|
> | `listening.png` | the button is held down | one hand cupped behind the ear, that shoulder raised |
> | `thinking.png` | waiting for a reply | a knuckle under where the chin will be |
> | `speaking.png` | talking | one open hand at chest height, just inside the frame |
> | `happy.png` | a cheerful reply | a small wave at head height, both shoulders up |
> | `sleepy.png` | idle for a long time | shoulders dropped, no hands |
> | `confused.png` | it did not understand | one shoulder up, one palm turned out at the frame edge |
>
> Same canvas, same transparent background, still nothing above the neck.
>
> Deliver six files in `body/`.

---

## B. Head angles — four heads and their features

The head turns to react and faces forward to emote, so an angled head needs
only the parts that animate. Ask for one angle at a time, all its parts
together, so the eyes and the head they belong to are drawn in one sitting.

### B1 — the angled head

> Here is `base/front.png` and a guide. Draw the same head turned **15 degrees
> to its right** (the viewer's left), as `base/turn_right_15.png`.
>
> Everything the front file leaves out, this one leaves out too: **no eyes, no
> brows, no mouth, and no bangs crossing the eyes.** Bare skin where the eyes
> go.
>
> The head keeps its size. It rotates; it does not move up the canvas or grow.
> The neck and the shoulders stay where `body/idle.png` has them, because that
> is what this head sits on.
>
> Canvas 1280 x 960, background alpha 0, nothing else in the file.

### B2 — that angle's features

> Here is `base/turn_right_15.png`. Draw the features for it: **twelve files**,
> each on the same 1280 x 960 canvas, each transparent except for its own part,
> each positioned where it lands on *this* head - not where it lands on the
> front-facing one.
>
> `eyes/turn_right_15/` — `open`, `half`, `closed`, `look_left`, `look_right`.
> Both eyes in the same state in all five; the blink stages lower both lids by
> the same amount; the two gaze parts move only the iris inside an unchanged
> outline.
>
> `mouths/turn_right_15/` — `rest`, `small`, `medium`, `wide`. A size ramp, in
> that order: this is what lip sync steps through, so `small` must read as
> smaller than `medium` at a glance.
>
> `brows/turn_right_15/` — `neutral`, `happy`, `sad`.
>
> On a head turned away from the viewer the far eye is narrower and closer to
> the edge of the face than the near one. That asymmetry is the point of
> drawing them per angle; a mirrored pair looks like a sticker.
>
> `hair_front/turn_right_15.png` — the bangs that fall in front of this face.

Then repeat B1 and B2 for `turn_left_15`, `look_up` and `look_down`. Later,
`tilt_left` and `tilt_right`.

---

## Accepting a delivery

```bash
python tools/validate_assets.py assets/kizuna --only body
python tools/validate_assets.py assets/kizuna --only eyes
```

Canvas size, transparency, and - for eyes - that the pupils are on the line at
the right separation and that both eyes are in the same state outside the
winks.

Two things the validator cannot check, so check them by hand:

- **Nothing above the neck in `body/`.** Open a pose over `base/front.png`; if
  the pose has a chin in it, it will be sticking through the head.
- **The angled features belong to the angled head.** Composite
  `base/turn_right_15.png` with `eyes/turn_right_15/open.png` and look at it.
  A front-facing pair on a turned head is obvious once composited and invisible
  in the file list.
