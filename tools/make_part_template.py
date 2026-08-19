#!/usr/bin/env python3
"""Draws the guide image that a part request is handed alongside its prompt.

Written because coordinates in prose get misread. Three eye deliveries in a row
came back on the wrong canvas with the eyes up to 2.5x too far apart, each time
against a spec that stated every number. A picture of what the part sits on is
not open to interpretation, and it costs nothing to produce.

    python tools/make_part_template.py eyes          # what eyes register to
    python tools/make_part_template.py body          # what a pose registers to
    python tools/make_part_template.py base --over eyes,mouths

The reference is whatever has already been delivered under assets/kizuna, so
the guide always shows the artwork the new part has to sit with rather than a
second description of it that can drift.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eye_geometry as eg  # noqa: E402

GUIDE = (0, 170, 60)
BOX = (0, 120, 255)
CENTRE = (220, 60, 60)
NOTE = (200, 0, 0)

# Which layer wants which marks, and what the part is expected to leave alone.
LAYERS = {
    "eyes": ("the two eyes and nothing else",
             "pupils on the green marks, everything inside the blue box"),
    "brows": ("the two eyebrows and nothing else",
              "above the eye box, following the same head"),
    "mouths": ("the mouth and nothing else",
               "inside the blue box, on the mouth line"),
    "hair_front": ("only the hair that falls in front of the face",
                   "the rest of the hair stays in base/"),
    "base": ("the head, neck and the hair behind the face",
             "no eyes, no brows, no mouth, no bangs across the eyes"),
    "body": ("everything below the neck",
             "no head - it is composited on top"),
}


def load(path: Path) -> Image.Image | None:
    if not path.exists():
        return None
    return Image.open(path).convert("RGBA").resize(eg.CANVAS, Image.LANCZOS)


def reference(root: Path, angle: str) -> Image.Image:
    """The delivered artwork this part has to sit with, faded to a guide."""
    canvas = Image.new("RGBA", eg.CANVAS, (255, 255, 255, 0))
    for layer, name in (("body", "idle"), ("base", angle),
                        ("eyes", "open"), ("mouths", "rest"),
                        ("hair_front", angle)):
        sub = root / layer
        candidates = [sub / f"{name}.png", sub / angle / f"{name}.png"]
        for path in candidates:
            image = load(path)
            if image is not None:
                canvas.alpha_composite(image)
                break
    return canvas


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("layer", choices=sorted(LAYERS))
    ap.add_argument("--angle", default="front")
    ap.add_argument("--assets", type=Path, default=root / "assets/kizuna")
    ap.add_argument("--pack", type=Path, default=root / "build/sd/companion/packs/kizuna")
    ap.add_argument("--out", type=Path, default=None)
    a = ap.parse_args()

    ref = reference(a.assets, a.angle)
    if np.asarray(ref)[:, :, 3].max() == 0:
        # Nothing delivered yet - fall back to the built pack, which at least
        # shows the head at the size and place the device actually uses.
        ref = eg.first_frame(a.pack / "base" / "neutral.m5a").convert("RGBA")
        ref = ref.resize(eg.CANVAS, Image.LANCZOS)

    tpl = Image.blend(Image.new("RGB", eg.CANVAS, (255, 255, 255)),
                      Image.alpha_composite(Image.new("RGBA", eg.CANVAS, (255, 255, 255, 255)),
                                            ref).convert("RGB"), 0.42)
    d = ImageDraw.Draw(tpl)

    left, right = eg.target_pupils(a.pack)
    line = round((left[1] + right[1]) / 2)
    ex0, ey0, ex1, ey1 = 384, 328, 896, 504          # eye_rect x4
    mx0, my0, mx1, my1 = 512, 496, 768, 656          # mouth_rect x4

    d.line([eg.CANVAS[0] // 2, 0, eg.CANVAS[0] // 2, eg.CANVAS[1]], fill=CENTRE, width=2)
    if a.layer in ("eyes", "base", "brows"):
        for px, py in (left, right):
            d.line([px, py - 130, px, py + 130], fill=GUIDE, width=3)
        d.line([ex0 - 190, line, ex1 + 190, line], fill=GUIDE, width=3)
        d.rectangle((ex0, ey0, ex1, ey1), outline=BOX, width=4)
        d.text((ex0 + 8, ey0 - 26), f"eye box {ex0}..{ex1} x {ey0}..{ey1}", fill=BOX)
        d.text((left[0] - 90, line + 140),
               f"pupils x={round(left[0])} / x={round(right[0])}, y={line}", fill=GUIDE)
    if a.layer in ("mouths", "base"):
        d.rectangle((mx0, my0, mx1, my1), outline=BOX, width=4)
        d.text((mx0 + 8, my0 - 26), "mouth box", fill=BOX)
    if a.layer == "body":
        d.line([0, my1 + 40, eg.CANVAS[0], my1 + 40], fill=BOX, width=3)
        d.text((16, my1 + 46), "draw below this line only - the head is a layer",
               fill=BOX)

    # The device's text panel covers the bottom 224 px of this canvas. Anything
    # composed down there is never seen.
    band = 736
    d.rectangle((0, band, eg.CANVAS[0] - 1, eg.CANVAS[1] - 1),
                fill=(24, 26, 32), outline=(90, 95, 110), width=3)
    d.text((14, band + 10), "text panel - the device draws over this, "
                            "keep the character above y=736", fill=(210, 215, 230))

    draws, leaves = LAYERS[a.layer]
    d.text((20, 20), f"{a.layer}/{a.angle}: draw {draws}", fill=NOTE)
    d.text((20, 44), f"{leaves}", fill=NOTE)
    d.text((20, 68), "GUIDE ONLY - remove this layer before exporting", fill=NOTE)

    out = a.out or (a.assets / "_review" / f"template_{a.layer}_{a.angle}.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    tpl.save(out)
    print(f"{out}  {tpl.size[0]}x{tpl.size[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
