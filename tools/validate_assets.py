#!/usr/bin/env python3
"""Checks a delivered asset set against docs/ASSET_BRIEF.md.

Run this before the artwork goes anywhere near the packer. Every failure it
reports is one that would otherwise turn into a floating eye, a white halo or
a missing expression on a device you can only inspect by squinting at a 2.4
inch screen.

    python tools/validate_assets.py assets/kizuna
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CANVAS = (1280, 960)
EYE_LINE_Y = 448
INTEROCULAR = 192
FACE_CENTER_X = 640

LAYER_DIRS = ("base", "eyes", "mouths", "brows", "fx")


class Report:
    def __init__(self):
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, msg: str):
        self.errors.append(msg)

    def warn(self, msg: str):
        self.warnings.append(msg)

    def summary(self) -> int:
        for w in self.warnings:
            print(f"  warn  {w}")
        for e in self.errors:
            print(f"  FAIL  {e}")
        print(f"\n{len(self.errors)} error(s), {len(self.warnings)} warning(s)")
        return 1 if self.errors else 0


def check_image(path: Path, rel: str, report: Report):
    try:
        image = Image.open(path)
    except Exception as exc:
        report.error(f"{rel}: cannot open ({exc})")
        return None

    if image.mode != "RGBA":
        report.error(f"{rel}: mode is {image.mode}, expected RGBA")
        return None
    if image.size != CANVAS:
        report.error(f"{rel}: size is {image.size}, expected {CANVAS}")
        return None

    arr = np.asarray(image)
    alpha = arr[:, :, 3]

    if alpha.min() > 250:
        report.error(f"{rel}: fully opaque - the background was baked in")
        return arr

    coverage = (alpha > 40).mean()
    if coverage < 0.001:
        report.error(f"{rel}: essentially empty")
    elif coverage > 0.92:
        report.warn(f"{rel}: covers {coverage:.0%} of the canvas; check for a "
                    "background rectangle")

    # A pale fringe is what is left when a white background was erased by
    # threshold rather than drawn transparent. Look at partially transparent
    # pixels only: if they are consistently near-white, that is the fringe.
    edge = (alpha > 20) & (alpha < 200)
    if edge.sum() > 500:
        rgb = arr[:, :, :3][edge].astype(np.float32)
        if rgb.mean() > 236:
            report.warn(f"{rel}: soft edges are near-white - possible halo from "
                        "a removed background")
    return arr


def check_eye_registration(arr: np.ndarray, rel: str, report: Report):
    """Pupils should sit on the eye line, the right distance apart."""
    alpha = arr[:, :, 3]
    ys, xs = np.nonzero(alpha > 60)
    if xs.size < 100:
        return

    cy = float(ys.mean())
    if abs(cy - EYE_LINE_Y) > 60:
        report.error(f"{rel}: eye content centred at y={cy:.0f}, "
                     f"expected near {EYE_LINE_Y}")

    left = xs[xs < FACE_CENTER_X]
    right = xs[xs >= FACE_CENTER_X]
    if left.size < 50 or right.size < 50:
        report.warn(f"{rel}: only one eye found - fine for a wink, otherwise a bug")
        return

    span = float(right.mean() - left.mean())
    if abs(span - INTEROCULAR) > 40:
        report.warn(f"{rel}: eyes about {span:.0f} px apart, expected "
                    f"~{INTEROCULAR}")

    if "wink" not in Path(rel).stem:
        check_eye_symmetry(alpha, rel, report)


def check_eye_symmetry(alpha: np.ndarray, rel: str, report: Report):
    """Both eyes should be in the same state unless the part is a wink.

    A blink stage that closes one eye further than the other reads as a wink,
    and blink fires several times a minute - so the character appears to wink
    at the viewer constantly. Mirroring one half onto the other and comparing
    coverage catches it without needing to know what the eye looks like.
    """
    left = alpha[:, :FACE_CENTER_X] > 60
    right = np.fliplr(alpha[:, FACE_CENTER_X:]) > 60
    n = min(left.shape[1], right.shape[1])
    # Compare the halves inward from the centre line, where the eyes are.
    la = int(left[:, left.shape[1] - n:].sum())
    ra = int(right[:, right.shape[1] - n:].sum())
    if la < 50 or ra < 50:
        return
    ratio = min(la, ra) / max(la, ra)
    if ratio < 0.6:
        report.error(f"{rel}: the two eyes differ in coverage by "
                     f"{100 * (1 - ratio):.0f}% - one is more closed than the "
                     f"other, which reads as a wink")


def check_base(arr: np.ndarray, rel: str, report: Report):
    """A base face must not already have eyes drawn on it."""
    rgb = arr[:, :, :3].astype(np.int32)
    alpha = arr[:, :, 3]
    band = slice(EYE_LINE_Y - 50, EYE_LINE_Y + 50)
    r, g, b = rgb[band, :, 0], rgb[band, :, 1], rgb[band, :, 2]
    # The irises are the only cool-toned thing on this character; skin runs an
    # R-B of about +65 and the hair about +15.
    iris = (b - r > 6) & (r + g + b > 90) & (r + g + b < 600) & (alpha[band] > 60)
    if iris.sum() > 400:
        report.error(f"{rel}: eyes appear to be drawn on the base face")


def check_metadata(root: Path, report: Report):
    path = root / "character.json"
    if not path.exists():
        report.error("character.json is missing")
        return

    try:
        meta = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        report.error(f"character.json: {exc}")
        return

    expressions = meta.get("expressions", {})
    if len(expressions) < 10:
        report.warn(f"character.json: only {len(expressions)} expressions defined")

    referenced: set[str] = set()

    def want(layer: str, name, where: str):
        if not name:
            return
        for n in (name if isinstance(name, list) else [name]):
            rel = f"{layer}/{n}.png"
            referenced.add(rel)
            if not (root / rel).exists():
                report.error(f"{where} refers to missing {rel}")

    for name, spec in expressions.items():
        if not spec.get("meaning"):
            report.warn(f"expression '{name}' has no meaning - the server uses it "
                        "to choose a face")
        want("base", spec.get("base"), f"expression '{name}'")
        want("eyes", spec.get("eyes"), f"expression '{name}'")
        want("mouths", spec.get("mouth"), f"expression '{name}'")
        want("brows", spec.get("brows"), f"expression '{name}'")
        want("fx", spec.get("fx"), f"expression '{name}'")

    for seq_name, seq in meta.get("sequences", {}).items():
        layer = {"eyes": "eyes", "mouth": "mouths", "base": "base"}.get(seq.get("layer"))
        for frame in seq.get("frames", []):
            part = frame[0] if isinstance(frame, list) else frame
            if layer:
                want(layer, part, f"sequence '{seq_name}'")

    blink = meta.get("sequences", {}).get("blink", {})
    if blink and len({f[0] if isinstance(f, list) else f for f in blink.get("frames", [])}) < 4:
        report.warn("the blink sequence has fewer than four distinct stages; it "
                    "will read as a light switch")

    for layer in LAYER_DIRS:
        for png in sorted((root / layer).glob("*.png")) if (root / layer).is_dir() else []:
            rel = f"{layer}/{png.name}"
            if rel not in referenced:
                report.warn(f"{rel} is never referenced by character.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", type=Path, help="the delivered character directory")
    ap.add_argument("--only", nargs="+", metavar="LAYER", choices=LAYER_DIRS,
                    help="check just these layers, for a delivery that replaces "
                         "one of them; character.json is then not required")
    args = ap.parse_args()

    if not args.root.is_dir():
        raise SystemExit(f"not a directory: {args.root}")

    report = Report()
    counted = 0
    layers = tuple(args.only) if args.only else LAYER_DIRS

    for layer in layers:
        directory = args.root / layer
        if not directory.is_dir():
            report.error(f"missing directory: {layer}/")
            continue
        files = sorted(directory.glob("*.png"))
        if not files:
            report.error(f"{layer}/ contains no PNG files")
        print(f"{layer}/  {len(files)} file(s)")
        for png in files:
            rel = f"{layer}/{png.name}"
            arr = check_image(png, rel, report)
            counted += 1
            if arr is None:
                continue
            if layer == "eyes":
                check_eye_registration(arr, rel, report)
            elif layer == "base":
                check_base(arr, rel, report)

    print(f"\nchecked {counted} image(s)")
    if not args.only:
        check_metadata(args.root, report)
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
