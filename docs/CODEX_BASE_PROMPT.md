# Codex prompt — Kizuna base faces

Self-contained, like [CODEX_EYES_PROMPT.md](CODEX_EYES_PROMPT.md). Twelve base
faces: the character with **no eyes, no brows and no mouth**, which the device
composites those parts onto.

Two things make this delivery different from the eyes, and both matter more
than the drawing:

1. **The body must be pixel-identical in all twelve.** Only the head is
   re-posed. This is not a stylistic preference - it is what makes head
   motion watchable. The device now sends only the 16x16 tiles that changed
   since the previous frame, which took gestures from 5.5 fps to a measured
   11.7 fps on the current artwork. Of what is left, **36% is the body moving
   when it did not need to**; freezing it should reach about 18 fps.
2. **No eyes, no brows, no mouth.** The current pack's bases are finished
   faces with eyes drawn on, which is why new eye artwork does not sit on
   them - it lands on top of the old eyes instead of replacing them.

Hand over `assets/kizuna/_review/eye_template.png` with the prompt so the head
lands where the device expects it. Regenerate it with
`python tools/make_eye_template.py`.

---

## Step 1 — the master

> Draw the front-facing base for a character called **Kizuna**, for a desktop
> companion robot.
>
> He is a semi-realistic anime boy: dark brown wolf-cut hair with orange inner
> streaks, an orange-lined black hoodie jacket over a white tee, a black
> choker, and a white "M5" hair clip above his right eye. Clean line art, soft
> cel shading, head and shoulders, facing the viewer.
>
> **Draw the face with no eyes, no eyebrows and no mouth.** Eyelids, lashes
> and the eye sockets are not drawn either - leave bare skin where the eyes
> go. Those parts are separate files that the device composites on top, and
> anything drawn here shows through them.
>
> Technical requirements, all of them mandatory:
>
> - Canvas exactly **1280 x 960 px**, RGBA PNG. Not 1448x1086, not 1536x1024.
> - The background is **fully transparent** - alpha 0, not a low-alpha wash.
>   The device draws its own backdrop behind the character.
> - Register the head to the attached `eye_template.png`: the head fills the
>   same area, at the same size, in the same place. The eyes it shows are the
>   ones you are leaving out, so put the bare eye area where they are.
> - No captions, numbers, frames, labels or watermarks anywhere in the image.
>
> Deliver **one file**, `base/front.png`. Not a contact sheet.

---

## Step 2 — the other eleven, as edits of the master

> Here is `base/front.png`. Produce the remaining eleven base faces by
> **editing this image**.
>
> **The body must not move.** Everything from the neck down - the choker, the
> jacket, the collar, the shoulders - is pixel-identical to `front.png` in
> every one of the twelve files. Do not redraw it, do not shift it, do not
> re-shade it. Only the head, the hair and the neck change.
>
> That constraint is the whole point of this step. The device redraws only the
> 16x16 tiles that differ from the previous frame, and a body that moves is
> 36% of that budget - measured, on the pack this replaces. Freezing it is
> worth about six frames a second on its own, which is the difference between
> a nod and a slideshow.
>
> The eleven:
>
> | File | Pose |
> |---|---|
> | `tilt_left.png` / `tilt_right.png` | head tilted about 12 degrees, ear toward the shoulder |
> | `turn_left_15.png` / `turn_right_15.png` | head turned 15 degrees |
> | `turn_left_30.png` / `turn_right_30.png` | head turned 30 degrees |
> | `look_up.png` / `look_down.png` | chin raised / lowered, head otherwise square on |
> | `nod_up.png` / `nod_down.png` | the extremes of a nod - a small vertical shift, less than the look_up/look_down tilt |
> | `idle.png` | the resting pose, a hair's breadth off `front.png` |
>
> Every one is the same 1280x960 canvas, transparent background, no eyes, no
> brows, no mouth. Keep the head's displacement small - these are the frames a
> gesture is interpolated through, not separate illustrations.
>
> Deliver eleven files in `base/`.

---

## Accepting the delivery

```bash
python tools/validate_assets.py assets/kizuna --only base
```

It fails a file that is not 1280x960 RGBA, that has an opaque or low-alpha
background, or that has eyes drawn on it. Check the frozen body yourself -
open two of them as layers and difference them; everything below the neck
should be black.
