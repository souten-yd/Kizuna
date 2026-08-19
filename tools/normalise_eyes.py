#!/usr/bin/env python3
"""Puts a delivered eye PNG onto the canvas the packer expects.

An image generator does not hit pixel coordinates. Three deliveries in a row
came back on a canvas that was not 1280x960, with the irises between 1.5x and
2.5x too far apart, while following a written spec that stated every number.
Iterating on that is a waste of everyone's time, and it is not necessary: a
pair of eyes on a transparent canvas needs one uniform scale and one
translation per eye to be exactly right, and both can be computed.

    python tools/normalise_eyes.py assets/kizuna/eyes/open.png

This is not the registration that failed before. That one tried to align
twenty-two independently drawn parts against a face, and lost because the
parts differed in size from each other. Here exactly one file is corrected -
the master - and the other twenty-one are produced by editing the corrected
master, so they inherit its registration rather than each needing their own.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eye_geometry as eg  # noqa: E402


def normalise(src: Image.Image, targets) -> Image.Image:
    src = src.convert("RGBA")
    if src.size != eg.CANVAS:
        # Aspect is usually right and the absolute size wrong; either way the
        # scale below is derived from the irises, so this only sets the frame.
        src = src.resize(eg.CANVAS, Image.LANCZOS)

    alpha = np.asarray(src)[:, :, 3]
    found = eg.iris_centres(src, alpha)

    span_src = found[1][0] - found[0][0]
    span_dst = targets[1][0] - targets[0][0]
    scale = span_dst / span_src

    scaled = src.resize((max(1, round(eg.CANVAS[0] * scale)),
                         max(1, round(eg.CANVAS[1] * scale))), Image.LANCZOS)

    # Split the canvas between the eyes and place each half by its own iris, so
    # a head drawn with the eyes too wide apart is closed up rather than
    # squashed. Each eye keeps its shape; only where it sits changes.
    boundary = round((targets[0][0] + targets[1][0]) / 2)
    out = Image.new("RGBA", eg.CANVAS, (0, 0, 0, 0))
    for i, (tx, ty) in enumerate(targets):
        dx = round(tx - found[i][0] * scale)
        dy = round(ty - found[i][1] * scale)
        layer = Image.new("RGBA", eg.CANVAS, (0, 0, 0, 0))
        layer.paste(scaled, (dx, dy))
        keep = Image.new("L", eg.CANVAS, 0)
        box = (0, 0, boundary, eg.CANVAS[1]) if i == 0 else \
              (boundary, 0, eg.CANVAS[0], eg.CANVAS[1])
        keep.paste(255, box)
        out.paste(layer, (0, 0), Image.composite(layer.split()[3], keep.point(lambda _: 0), keep))
    return out


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=Path, help="the delivered PNG")
    ap.add_argument("-o", "--out", type=Path, help="default: alongside, .normalised.png")
    ap.add_argument("--pack", type=Path, default=root / "build/sd/companion/packs/kizuna")
    ap.add_argument("--expression", default="neutral")
    a = ap.parse_args()

    targets = eg.target_pupils(a.pack, a.expression)
    src = Image.open(a.source)
    print(f"in   {a.source.name}  {src.size[0]}x{src.size[1]}")

    before = eg.iris_centres(src.convert("RGBA").resize(eg.CANVAS, Image.LANCZOS),
                             np.asarray(src.convert("RGBA").resize(eg.CANVAS, Image.LANCZOS))[:, :, 3])
    print(f"     irises {before[1][0] - before[0][0]:.0f} px apart, "
          f"y {(before[0][1] + before[1][1]) / 2:.0f}")
    print(f"want irises {targets[1][0] - targets[0][0]:.0f} px apart, "
          f"y {(targets[0][1] + targets[1][1]) / 2:.0f}")

    out = normalise(src, targets)
    after = eg.iris_centres(out, np.asarray(out)[:, :, 3])
    print(f"out  {out.size[0]}x{out.size[1]}  irises "
          f"{after[1][0] - after[0][0]:.0f} px apart, "
          f"y {(after[0][1] + after[1][1]) / 2:.0f}")
    for i, name in enumerate(("left", "right")):
        print(f"     {name} iris off target by "
              f"({after[i][0] - targets[i][0]:+.1f}, {after[i][1] - targets[i][1]:+.1f}) px")

    dest = a.out or a.source.with_suffix(".normalised.png")
    out.save(dest)
    print(f"wrote {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
