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

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m5a

SCREEN = (320, 240)
BACKDROP = (18, 18, 22)

# Measured 2026-08-20 from master vs eye_closed / mouth_wide, restricted to the
# region the user marked. The eyes cost 3.3 KB and the mouth 11.6 KB per blit
# against a bus that carries roughly 850 KB/s.
EYE_RECT = (122, 90, 72, 23)
MOUTH_RECT = (117, 115, 97, 60)

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


def load(src: Path, name: str) -> Image.Image:
    """One variant, flattened onto the backdrop and scaled to the screen."""
    im = Image.open(src / f"{name}.png").convert("RGBA")
    flat = Image.alpha_composite(Image.new("RGBA", im.size, BACKDROP + (255,)), im)
    return flat.convert("RGB").resize(SCREEN, Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=Path("assets/kizuna/variants"))
    ap.add_argument("--out", type=Path, default=Path("build/sd/companion/packs/kizuna"))
    ap.add_argument("--name", default="kizuna")
    a = ap.parse_args()

    frames = {}
    for _, name in EYE_SLOTS + VISEMES:
        if name not in frames:
            frames[name] = load(a.src, name)

    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    total = 0

    total += m5a.write_clip(out / "base" / "neutral.m5a", [frames["master"]])

    ex, ey, ew, eh = EYE_RECT
    eyes = [frames[n].crop((ex, ey, ex + ew, ey + eh)) for _, n in EYE_SLOTS]
    total += m5a.write_clip(out / "eyes" / "neutral.m5a", eyes)

    mx, my, mw, mh = MOUTH_RECT
    mouth = [frames[n].crop((mx, my, mx + mw, my + mh)) for _, n in VISEMES]
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

    print(f"{a.name}: {total / 1024:.0f} KB in {out}")
    print(f"  eyes  {ew}x{eh} = {ew * eh * 2} B/blit -> {845070 / (ew * eh * 2):.0f} fps")
    print(f"  mouth {mw}x{mh} = {mw * mh * 2} B/blit -> {845070 / (mw * mh * 2):.0f} fps")
    print(f"  {len(EYE_SLOTS)} eye slots, {len(VISEMES)} visemes, "
          f"{len(manifest['expressions'])} expressions, no gesture clips")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
