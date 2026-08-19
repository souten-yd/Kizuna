#!/usr/bin/env python3
"""Draws the registration template that eye artwork is produced against.

Coordinates in prose are something to get wrong. Two deliveries in a row came
back with the eyes about 2.5x too far apart and twice the size, on a canvas
that was neither the requested one nor the same shape twice - while following
a written spec that stated all three numbers. A picture of the face the eyes
have to land on, at the size the device uses, is not open to interpretation.

    python tools/make_eye_template.py

The face comes from a built pack, so the template always shows the geometry
the packer is actually using rather than a second copy of it that can drift.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SCREEN = (320, 240)
CANVAS = (1280, 960)
SCALE = CANVAS[0] // SCREEN[0]

GUIDE = (0, 170, 60)
BOX = (0, 120, 255)
CENTRE = (220, 60, 60)


def first_frame(path: Path) -> Image.Image:
    data = path.read_bytes()
    _, _, _, w, h, _, _, frame_bytes, fmt = struct.unpack("<IHHHHHHIB", data[:21])
    v = np.frombuffer(data[32:32 + frame_bytes],
                      dtype=">u2" if fmt == 0 else "<u2").reshape(h, w).astype(np.uint32)
    rgb = np.dstack([((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3])
    return Image.fromarray(rgb.astype(np.uint8))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parents[1]
    ap.add_argument("--pack", type=Path, default=root / "build/sd/companion/packs/kizuna")
    ap.add_argument("--expression", default="neutral")
    ap.add_argument("--out", type=Path, default=root / "assets/kizuna/_review/eye_template.png")
    a = ap.parse_args()

    recipe = __import__("json").loads((root / "assets/characters/kizuna.json").read_text())
    ex, ey, ew, eh = recipe["geometry"]["eye_rect"]
    span = recipe["geometry"]["eye_span"]
    cx_screen = recipe["geometry"]["eye_center"][0]
    eye_line = recipe["geometry"]["eye_center"][1]

    base = first_frame(a.pack / "base" / f"{a.expression}.m5a")
    base.paste(first_frame(a.pack / "eyes" / f"{a.expression}.m5a"), (ex, ey))
    face = base.resize(CANVAS, Image.LANCZOS)

    # Faded: this is a guide to draw against, not artwork to trace.
    tpl = Image.blend(Image.new("RGB", CANVAS, (255, 255, 255)), face, 0.42)
    d = ImageDraw.Draw(tpl)

    cx, line = cx_screen * SCALE, eye_line * SCALE
    half = span * SCALE // 2
    box = (ex * SCALE, ey * SCALE, (ex + ew) * SCALE, (ey + eh) * SCALE)

    for pupil in (cx - half, cx + half):
        d.line([pupil, line - 130, pupil, line + 130], fill=GUIDE, width=3)
    d.line([box[0] - 190, line, box[2] + 190, line], fill=GUIDE, width=3)
    d.rectangle(box, outline=BOX, width=4)
    d.line([cx, 0, cx, CANVAS[1]], fill=CENTRE, width=2)

    d.text((box[0] + 8, box[1] - 28),
           f"eye_rect {box[0]}..{box[2]} x {box[1]}..{box[3]} - "
           "everything you draw fits inside this", fill=BOX)
    d.text((cx - 92, line + 140),
           f"pupil centres x={cx - half} / x={cx + half}, y={line}", fill=GUIDE)
    d.text((20, 20), "GUIDE ONLY - remove this layer before exporting",
           fill=CENTRE)

    a.out.parent.mkdir(parents=True, exist_ok=True)
    tpl.save(a.out)
    print(f"{a.out}  {tpl.size[0]}x{tpl.size[1]}")
    print(f"  pupils x={cx - half} and x={cx + half}, eye line y={line}")
    print(f"  eye_rect {box}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
