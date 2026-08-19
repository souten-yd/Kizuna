"""Where the eyes actually are, measured from a built pack.

The recipe's `eye_span` is what the packer normalises *towards*, using a
landmark detector whose idea of an eye centre is a bounding box rather than a
pupil. The composed artwork therefore ends up with its irises a different
distance apart than that number suggests - 237 px against 192 on the 1280
canvas, for the pack this was written against. Artwork has to match the
picture, not the intent, so everything that talks to an artist measures the
picture.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
from PIL import Image

SCREEN = (320, 240)
CANVAS = (1280, 960)
SCALE = CANVAS[0] // SCREEN[0]


def first_frame(path: Path, index: int = 0) -> Image.Image:
    data = path.read_bytes()
    _, _, _, w, h, _, _, fb, fmt = struct.unpack("<IHHHHHHIB", data[:21])
    v = np.frombuffer(data[32 + index * fb:32 + (index + 1) * fb],
                      dtype=">u2" if fmt == 0 else "<u2").reshape(h, w).astype(np.uint32)
    rgb = np.dstack([((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3])
    return Image.fromarray(rgb.astype(np.uint8))


def iris_centres(image: Image.Image, alpha: np.ndarray | None = None):
    """The two iris centres, in the image's own pixels.

    The irises are the only cool-toned thing on this character: skin runs an
    R-B of about +65 and the hair about +15, so a blue bias picks them out
    without needing to know what an eye looks like.
    """
    a = np.asarray(image.convert("RGB")).astype(int)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    mask = (b - r > 4) & (r + g + b < 560)
    if alpha is not None:
        mask &= alpha > 128
    ys, xs = np.nonzero(mask)
    if xs.size < 40:
        raise ValueError("no irises found")

    # Split at the widest empty column between the two, not at the midpoint of
    # the image: a delivery may be off-centre.
    order = np.argsort(xs)
    xs_sorted = xs[order]
    gaps = np.diff(xs_sorted)
    split = xs_sorted[int(np.argmax(gaps))] + int(gaps.max()) // 2

    left = xs < split
    return ((float(xs[left].mean()), float(ys[left].mean())),
            (float(xs[~left].mean()), float(ys[~left].mean())))


def target_pupils(pack: Path, expression: str = "neutral"):
    """Pupil centres on the 1280x960 canvas.

    Prefers `assets/kizuna/eye_anchor.json`, which holds the positions a person
    read off a magnified tile. `iris_centres` is the fallback for a pack that
    has no anchor file yet, and it is only approximately right - the iris runs
    from dark at the top to pale blue at the bottom, so any centroid of it sits
    below the pupil. It put the eyes about 20 px low, which is visible.
    """
    import json

    root = Path(__file__).resolve().parents[1]
    anchor = root / "assets/kizuna/eye_anchor.json"
    if anchor.exists():
        a = json.loads(anchor.read_text())
        if tuple(a["canvas"]) == CANVAS:
            return tuple(a["left_pupil"]), tuple(a["right_pupil"])

    geom = json.loads((root / "assets/characters/kizuna.json").read_text())["geometry"]
    ex, ey = geom["eye_rect"][0], geom["eye_rect"][1]
    tile = first_frame(pack / "eyes" / f"{expression}.m5a", 0)
    (lx, ly), (rx, ry) = iris_centres(tile)
    return (((ex + lx) * SCALE, (ey + ly) * SCALE),
            ((ex + rx) * SCALE, (ey + ry) * SCALE))
