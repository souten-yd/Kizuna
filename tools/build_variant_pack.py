#!/usr/bin/env python3
"""Build a companion pack from hand-drawn diff variants.

Every source here is a full picture of the character that differs from the
master in one feature. The packer keeps only the rectangle that feature lives
in and throws the rest away, so a generator that redrew the whole head while
changing the mouth still yields a usable part - the redrawing simply never
reaches the screen. That is what lets these deliveries be used as they arrived.

The rectangles are deliberately hand-set rather than derived per variant. A
rectangle measured from one variant and applied to all of them keeps every
frame of a feature in register with every other, which is what stops the seam
that appears when each frame carries its own box.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m5a
import register

SCREEN = (320, 240)
BACKDROP = (18, 18, 22)

# Measured 2026-08-20 against blank_face, which has no eyes and no mouth, so the
# difference against it is the feature itself rather than the change between two
# states of it. The first attempt took the eye rectangle from master vs
# eye_closed and clipped 14 px off the top of the open eyes, because a closed
# lid does not reach as high as an open one. A rectangle has to hold every state
# the feature will ever take, and only a picture with the feature absent shows
# where that is.
#
# Hair is excluded by requiring the base to be skin at that pixel; the two
# pictures are separate renders, so their hair strands differ everywhere and
# would otherwise claim the whole head.
EYE_RECT = (113, 70, 108, 49)
MOUTH_RECT = (140, 119, 62, 33)

# Firmware's fixed slot order.
# eye_half as delivered is a wink - one eye shut, not both lids lowered - so it
# cannot stand in for the middle of a blink. Cross-fading open and closed was
# tried instead and the iris ghosts through as a translucent disc, because a
# fade is not a lid coming down. The blink is therefore two stages, which at
# 255 fps per blit passes too quickly to read as a jump. A genuine both-eyes-
# half picture is part 09 in docs/PARTS_LIST.md; when it arrives the three
# intermediate slots below take it and the blink softens without any code
# change. The wink keeps no slot until the firmware has one to give it.
EYE_SLOTS = [
    ("open_center", "master"), ("open_left", "eye_left"), ("open_right", "eye_right"),
    ("open_up", "master"), ("open_down", "master"), ("soft_lower", "eye_closed"),
    ("half", "eye_closed"), ("almost_closed", "eye_closed"), ("closed", "eye_closed"),
    ("wide", "master"), ("sleepy_half", "eye_closed"), ("sleepy_closed", "eye_closed"),
]
VISEMES = [
    ("rest", "master"), ("tiny", "mouth_small"), ("small", "mouth_small"),
    ("medium", "mouth_medium"), ("wide", "mouth_wide"), ("rounded", "mouth_medium"),
    ("smile_closed", "master"), ("smile_open", "mouth_wide"),
]


def load(src: Path, name: str, ref: Image.Image | None = None):
    """One variant, lined up with the master, flattened and scaled.

    The alignment is not optional. These pictures were re-rendered rather than
    edited, so each came back at its own offset - the base at +2.0 screen px and
    the mouths at -1.0 - and a mouth cut from one and drawn onto the other lands
    3 px off the face. Correcting on the full canvas puts the residual error
    below a quarter of a screen pixel.
    """
    im = Image.open(src / f"{name}.png").convert("RGBA")
    dx = dy = 0
    if ref is not None:
        if im.size != ref.size:
            im = im.resize(ref.size, Image.LANCZOS)
        im, dx, dy = register.align(ref, im)
    flat = Image.alpha_composite(Image.new("RGBA", im.size, BACKDROP + (255,)), im)
    return flat.convert("RGB").resize(SCREEN, Image.LANCZOS), dx, dy


def cut(src: Image.Image, base: Image.Image, rect, feather: int,
        soft: str = "tlrb") -> Image.Image:
    """The rectangle out of src, fading to base at its border.

    Registration puts the pictures within a quarter pixel of each other but it
    cannot make one generation's hair strands identical to another's, and the
    eye rectangle's border runs through hair. A hard edge there reads as a step
    twice the size of any change the drawing makes on its own. Fading the last
    few pixels into the base costs nothing on the device - the firmware still
    blits a plain rectangle - and the border stops being findable.
    """
    x, y, w, h = rect
    box = (x, y, x + w, y + h)
    part = np.asarray(src.crop(box)).astype(np.float32)
    under = np.asarray(base.crop(box)).astype(np.float32)
    if feather <= 0:
        return Image.fromarray(part.astype(np.uint8))
    # Only the edges that sit on hair are faded. The eye rectangle's bottom edge
    # runs across the cheek, where the two renders already agree, and fading
    # there ate the lower lash line - the feature looked cut off along its own
    # bottom. `soft` names the edges to fade.
    def ramp(n, lo, hi):
        r = np.ones(n)
        if lo:
            r = np.minimum(r, np.clip(np.arange(n) / feather, 0, 1))
        if hi:
            r = np.minimum(r, np.clip((n - 1 - np.arange(n)) / feather, 0, 1))
        return r
    a = np.minimum(ramp(w, "l" in soft, "r" in soft)[None, :],
                   ramp(h, "t" in soft, "b" in soft)[:, None])[..., None]
    return Image.fromarray(np.clip(under * (1 - a) + part * a, 0, 255).astype(np.uint8))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=Path("assets/kizuna/variants"))
    ap.add_argument("--out", type=Path, default=Path("build/sd/companion/packs/kizuna"))
    ap.add_argument("--name", default="kizuna")
    ap.add_argument("--feather", type=int, default=3,
                    help="pixels over which a part fades into the base at its "
                         "border. 0 for a hard edge.")
    ap.add_argument("--base", default="blank_face",
                    help="the picture the eye and mouth rectangles are drawn "
                         "onto. blank_face has no eyes and no mouth, so nothing "
                         "of the face survives underneath a part - a part can be "
                         "moved, resized or replaced later without the old "
                         "feature showing through it. With the rectangles fixed "
                         "as they are now the output is the same as using "
                         "master; the difference is what stays possible.")
    a = ap.parse_args()

    ref = Image.open(a.src / "master.png").convert("RGBA")
    frames, shifts = {}, {}
    for _, name in EYE_SLOTS + VISEMES + [(None, a.base)]:
        if name not in frames:
            frames[name], dx, dy = load(a.src, name, None if name == "master" else ref)
            shifts[name] = (dx, dy)

    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    total = 0

    total += m5a.write_clip(out / "base" / "neutral.m5a", [frames[a.base]])

    ex, ey, ew, eh = EYE_RECT
    eyes = [cut(frames[n], frames[a.base], EYE_RECT, a.feather, "tlr")
            for _, n in EYE_SLOTS]
    total += m5a.write_clip(out / "eyes" / "neutral.m5a", eyes)

    mx, my, mw, mh = MOUTH_RECT
    mouth = [cut(frames[n], frames[a.base], MOUTH_RECT, a.feather, "lrb")
             for _, n in VISEMES]
    total += m5a.write_clip(out / "mouth" / "neutral.m5a", mouth)

    # Every expression resolves, so no request can miss and leave a blank face.
    # They share one set of artwork until dedicated pictures exist.
    recipe = json.loads(Path("assets/characters/kizuna.json").read_text())
    entry = {"base": "base/neutral.m5a", "sway": "",
             "eyes": "eyes/neutral.m5a", "mouth": "mouth/neutral.m5a"}
    manifest = {
        "pack": a.name, "character": a.name, "version": 2, "format": "rgb565be",
        "theme": "light", "screen": {"w": SCREEN[0], "h": SCREEN[1]},
        "eye_center": {"x": ex + ew // 2, "y": ey + eh // 2},
        "sway_rect": {"x": 0, "y": 0, "w": 0, "h": 0}, "sway_frames": 1,
        "eye_rect": {"x": ex, "y": ey, "w": ew, "h": eh},
        "mouth_rect": {"x": mx, "y": my, "w": mw, "h": mh},
        "eye_slots": [s for s, _ in EYE_SLOTS],
        "viseme_frames": len(VISEMES), "visemes": [v for v, _ in VISEMES],
        "expressions": {name: dict(entry) for name in recipe["expressions"]},
        "clips": {},
    }
    text = json.dumps(manifest, ensure_ascii=False, indent=1)
    (out / "manifest.json").write_text(text)
    total += len(text)

    # The eyes and the mouth are redrawn independently between body frames
    # (DisplayTask.cpp), so overlapping rectangles let a blink repaint the top
    # of a mouth that is mid-word.
    if (ey + eh > my and ex < mx + mw and mx < ex + ew):
        raise SystemExit(f"eye rect {EYE_RECT} overlaps mouth rect {MOUTH_RECT}; "
                         "a blink would repaint the mouth")

    print(f"{a.name}: {total / 1024:.0f} KB in {out}")
    print(f"  eyes  {ew}x{eh} = {ew * eh * 2} B/blit -> {845070 / (ew * eh * 2):.0f} fps")
    print(f"  mouth {mw}x{mh} = {mw * mh * 2} B/blit -> {845070 / (mw * mh * 2):.0f} fps")
    scale = ref.size[0] / SCREEN[0]
    for name in sorted(shifts):
        dx, dy = shifts[name]
        if dx or dy:
            print(f"  aligned {name}: {dx:+d},{dy:+d} canvas px "
                  f"({dx / scale:+.2f},{dy / scale:+.2f} on screen)")
    print(f"  {len(EYE_SLOTS)} eye slots, {len(VISEMES)} visemes, "
          f"{len(manifest['expressions'])} expressions, no gesture clips")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
