#!/usr/bin/env python3
"""Crops and scales a whole delivery onto the canvas the packer expects.

An image generator will not produce a given pixel size. Three deliveries here
came back 4:3 to four decimal places against a request for 1280x960 and never
once at that size, and asking for the 40:23 the screen actually wants would be
pushing it further for nothing.

So do not ask. Let the artwork be drawn large, at whatever framing composes
naturally, and take the window this device needs out of it here. One transform
- a scale and a translation - is derived from the part that has the eyes in it,
and the same transform is applied to every other part. They share a canvas, so
they share a crop, and the composite comes out registered.

    python tools/normalise_parts.py assets/kizuna --out build/parts

The framing lives in this file rather than in the artwork, which means it can
be changed afterwards by re-running this rather than by commissioning anything.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eye_geometry as eg  # noqa: E402

LAYERS = ("base", "eyes", "brows", "mouths", "hair_front", "body", "fx")


def find_reference(root: Path) -> Path | None:
    """A part with both eyes in it, which is what the transform is measured on."""
    for candidate in ("eyes/open.png", "eyes/front/open.png", "eyes/open.normalised.png"):
        path = root / candidate
        if path.exists():
            return path
    found = sorted((root / "eyes").rglob("open.png"))
    return found[0] if found else None


def transform_from(reference: Path, targets) -> tuple[float, float, float]:
    """Scale and offset that put the reference's pupils on the target ones."""
    image = Image.open(reference).convert("RGBA")
    alpha = np.asarray(image)[:, :, 3]
    (lx, ly), (rx, ry) = eg.iris_centres(image, alpha)

    span_src = rx - lx
    span_dst = targets[1][0] - targets[0][0]
    if span_src <= 1:
        raise SystemExit(f"{reference}: the two eyes are not separable")
    scale = span_dst / span_src

    # Put the midpoint of the eyes where the midpoint of the targets is.
    mid_src = ((lx + rx) / 2, (ly + ry) / 2)
    mid_dst = ((targets[0][0] + targets[1][0]) / 2,
               (targets[0][1] + targets[1][1]) / 2)
    return scale, mid_dst[0] - mid_src[0] * scale, mid_dst[1] - mid_src[1] * scale


def apply(path: Path, scale: float, dx: float, dy: float) -> Image.Image:
    src = Image.open(path).convert("RGBA")
    scaled = src.resize((max(1, round(src.width * scale)),
                         max(1, round(src.height * scale))), Image.LANCZOS)
    out = Image.new("RGBA", eg.CANVAS, (0, 0, 0, 0))
    out.alpha_composite(scaled, (round(dx), round(dy)))
    return out


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("delivery", type=Path)
    ap.add_argument("--out", type=Path, default=root / "build/parts")
    ap.add_argument("--pack", type=Path, default=root / "build/sd/companion/packs/kizuna")
    ap.add_argument("--reference", type=Path, default=None,
                    help="the part the transform is measured on; default is eyes/open.png")
    a = ap.parse_args()

    reference = a.reference or find_reference(a.delivery)
    if reference is None:
        raise SystemExit(f"no eyes/open.png under {a.delivery} to measure against")

    targets = eg.target_pupils(a.pack)
    scale, dx, dy = transform_from(reference, targets)
    print(f"measured on {reference.relative_to(a.delivery)}")
    print(f"  scale {scale:.4f}, offset ({dx:+.0f}, {dy:+.0f}) -> {eg.CANVAS[0]}x{eg.CANVAS[1]}")

    if a.out.exists():
        shutil.rmtree(a.out)
    written = 0
    for layer in LAYERS:
        source = a.delivery / layer
        if not source.is_dir():
            continue
        for png in sorted(source.rglob("*.png")):
            out = a.out / png.relative_to(a.delivery)
            out.parent.mkdir(parents=True, exist_ok=True)
            apply(png, scale, dx, dy).save(out)
            written += 1
        print(f"  {layer}/  {len(list(source.rglob('*.png')))} file(s)")

    # Report where the eyes ended up, and whether anything is hidden behind the
    # text panel - the two things this is for.
    check = apply(reference, scale, dx, dy)
    (lx, ly), (rx, ry) = eg.iris_centres(check, np.asarray(check)[:, :, 3])
    print(f"\n{written} file(s) -> {a.out}")
    print(f"pupils now {rx - lx:.0f} px apart at y {(ly + ry) / 2:.0f}  "
          f"(target {targets[1][0] - targets[0][0]:.0f} at "
          f"{(targets[0][1] + targets[1][1]) / 2:.0f})")

    band = 736
    for layer in ("base", "body"):
        path = a.out / layer
        if not path.is_dir():
            continue
        for png in sorted(path.rglob("*.png")):
            alpha = np.asarray(Image.open(png))[:, :, 3]
            below = (alpha[band:] > 128).mean()
            if below > 0.25:
                print(f"  note: {png.name} has {below:.0%} of the panel area "
                      f"covered - that part is behind the text and never seen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
