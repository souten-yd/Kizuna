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
> on the device.
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
