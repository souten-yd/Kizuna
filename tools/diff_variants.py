#!/usr/bin/env python3
"""Finds what changed between a picture and its edited variants.

The registration problem this project spent a day on comes from compositing a
part that was drawn separately onto a face that was drawn separately. It
disappears if the variant is an *edit of the same picture*: whatever differs is
where the feature is, by construction, and no anchor has to be measured,
agreed or trusted.

So the artwork for one head angle is one full picture plus a handful of full
pictures identical to it except for the eyes. This works out the rectangle that
actually differs in each, which is what the packer stores and the device
pushes.

    python tools/diff_variants.py angles/turn_right_15

expects `base.png` and any number of other PNGs beside it, all the same size.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

# Below this, a pixel counts as unchanged. Generators re-encode the whole
# canvas even for a small edit, so a threshold of zero finds the whole picture.
THRESHOLD = 12
# Diff rectangles are rounded out to this, because the renderer pushes tiles.
GRID = 8


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def changed_rect(base: np.ndarray, variant: np.ndarray, threshold: int, grid: int):
    diff = np.abs(base - variant).max(axis=2) > threshold
    if not diff.any():
        return None, 0.0
    ys, xs = np.nonzero(diff)
    x0 = (xs.min() // grid) * grid
    y0 = (ys.min() // grid) * grid
    x1 = min(base.shape[1], ((xs.max() // grid) + 1) * grid)
    y1 = min(base.shape[0], ((ys.max() // grid) + 1) * grid)
    # How much of that rectangle is actually different, as a sanity check: a
    # tight, well-filled box is an edited feature, a sparse one spanning the
    # canvas is the generator having redrawn everything.
    inside = diff[y0:y1, x0:x1]
    return (int(x0), int(y0), int(x1), int(y1)), float(inside.mean())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("directory", type=Path)
    ap.add_argument("--base", default="base.png")
    ap.add_argument("--threshold", type=int, default=THRESHOLD)
    ap.add_argument("--grid", type=int, default=GRID)
    ap.add_argument("--screen", type=int, nargs=2, default=(320, 240),
                    metavar=("W", "H"), help="what the canvas becomes on the device")
    ap.add_argument("--json", type=Path, default=None)
    a = ap.parse_args()

    base_path = a.directory / a.base
    if not base_path.exists():
        raise SystemExit(f"no {a.base} in {a.directory}")
    base = load(base_path)
    h, w = base.shape[:2]
    sx, sy = a.screen[0] / w, a.screen[1] / h
    print(f"base {base_path.name}  {w}x{h} -> {a.screen[0]}x{a.screen[1]} on screen\n")

    print(f"{'variant':<22}{'changed rect (canvas)':>26}{'on screen':>14}{'bytes':>9}{'fill':>7}")
    out = {}
    for path in sorted(a.directory.glob("*.png")):
        if path.name == a.base:
            continue
        variant = load(path)
        if variant.shape != base.shape:
            print(f"{path.name:<22}  size {variant.shape[1]}x{variant.shape[0]} "
                  f"does not match the base - skipped")
            continue
        rect, fill = changed_rect(base, variant, a.threshold, a.grid)
        if rect is None:
            print(f"{path.name:<22}  identical to the base")
            continue
        x0, y0, x1, y1 = rect
        sw = round((x1 - x0) * sx)
        sh = round((y1 - y0) * sy)
        by = sw * sh * 2
        flag = ""
        if (x1 - x0) > w * 0.6 or (y1 - y0) > h * 0.6:
            flag = "  <- spans the canvas; the whole picture was redrawn"
        elif fill < 0.08:
            flag = "  <- sparse; something moved outside the feature"
        print(f"{path.name:<22}{f'{x0},{y0} {x1-x0}x{y1-y0}':>26}"
              f"{f'{sw}x{sh}':>14}{by:>9}{fill:>6.0%}{flag}")
        out[path.stem] = {"rect": rect, "screen": [sw, sh], "bytes": by, "fill": fill}

    if out:
        budget = 845070
        worst = max(v["bytes"] for v in out.values())
        print(f"\nlargest variant {worst} bytes -> {budget / worst:.0f} fps")
    if a.json:
        a.json.write_text(json.dumps(out, indent=2) + "\n")
        print(f"wrote {a.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
