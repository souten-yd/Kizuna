# Paste this to Codex

> Generate a complete layered character asset set for a desktop companion
> robot named **Kizuna**, running on an M5Stack M5GO with a 320x240 screen.
> The character is the existing "M5Stack CLAUDE Code" boy: dark brown wolf-cut
> hair with orange inner streaks, an orange-lined black hoodie jacket over a
> white tee, a black choker, and a white "M5" hair clip above his right eye.
> Blue-grey eyes. Semi-realistic anime style, clean line art, soft cel shading.
>
> Follow this specification exactly - it is a technical spec, not a
> suggestion. The failure modes it prevents are invisible until the artwork is
> on the device. Section 0 lists the ones a previous attempt actually hit, with
> the measurements; read it before you draw anything.
>
> **[paste the contents of docs/ASSET_BRIEF.md here]**
>
> Deliver:
>
> 1. The PNG files in the directory structure of section 2, all 1280x960
>    RGBA with transparent backgrounds, every part registered to the anchors
>    in section 1.
> 2. `character.json` per section 5, defining all thirty expressions from
>    section 5.1 with `meaning`, layer choices, `fx`, `idle` and `accent`, plus
>    the `sequences`, `transitions` and `visemes` blocks.
>
> Before you finish, verify against the checklist in section 7. In particular:
> base faces must have no eyes, no brows and no mouth; part files must not be
> cropped to their own bounding box; and nothing may have a white background.

## If only the eyes are being redrawn

The rest of the pack can stay as it is while the eyes are replaced, because
the eyes are where the visible defects were. Ask for `eyes/` alone, and say
this on top of the brief:

> Draw the twenty-two eye parts of section 3.2, and nothing else. Every one is
> a full 1280x960 RGBA canvas, transparent except for the two eyes, with the
> pupils on y = 448 and 192 px apart.
>
> Draw `open` first and treat it as the master. Every other part is that
> drawing modified - the same eye shape, the same size, in the same place on
> the canvas:
>
> - the four gaze parts move **only the iris and its highlight** inside an
>   unchanged outline. Do not redraw or resize the lid, the lash or the
>   eye opening. Gaze right was the worst defect in the previous pack.
> - the five blink stages lower **both lids by the same amount**. A stage that
>   closes one eye further than the other is read as a wink, and blink runs
>   several times a minute.
> - `wink_left` and `wink_right` are the only parts where the two eyes differ.
>
> Deliver them as layers of one drawing. If you render each part
> independently, the eye will change size between frames and the device will
> show debris along the lower lid every time the character blinks - which is
> exactly what happened last time.

Then verify before packing:

```bash
python tools/validate_assets.py assets/kizuna
```
