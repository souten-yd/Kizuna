#!/usr/bin/env python3
"""Line up variants that a generator returned at slightly different positions.

Every picture in a variant set is supposed to be the master with one feature
changed, but a generator re-renders rather than edits, and the result comes
back shifted by a pixel or two. On a 1448-wide canvas that is invisible; scaled
to a 320-wide screen and cut into rectangles it is not, because the rectangle
carries its own offset onto a base that has a different one. Measured on the
current set, the base sits +2.0 px from master and the mouths sit -1.0 px, so
the mouth lands 3 px off the face it is drawn onto.

The offset is measured on hair and clothing rather than the face, since the
face is what legitimately differs, and it is applied on the full-resolution
canvas so the correction survives the downscale as a fraction of a screen pixel.
"""

from __future__ import annotations

import numpy as np
from PIL import Image


def _grey(img: Image.Image, backdrop=(18, 18, 22)) -> np.ndarray:
    flat = Image.alpha_composite(Image.new("RGBA", img.size, backdrop + (255,)),
                                 img.convert("RGBA"))
    return np.asarray(flat.convert("L")).astype(np.float32)


def _search(ref: np.ndarray, mov: np.ndarray, mask: np.ndarray,
            centre: tuple[int, int], radius: int, step: int):
    best = None
    for dy in range(centre[1] - radius, centre[1] + radius + 1, step):
        for dx in range(centre[0] - radius, centre[0] + radius + 1, step):
            cand = np.roll(np.roll(mov, dy, axis=0), dx, axis=1)
            err = float(np.abs(ref - cand)[mask].mean())
            if best is None or err < best[0]:
                best = (err, dx, dy)
    return best


def offset(ref_img: Image.Image, mov_img: Image.Image,
           face: tuple[float, float, float, float] = (0.30, 0.30, 0.75, 0.80),
           radius: int = 14) -> tuple[int, int, float]:
    """How far mov sits from ref, in canvas pixels, ignoring the face box.

    `face` is the excluded region as fractions of width and height.
    """
    ref, mov = _grey(ref_img), _grey(mov_img)
    if ref.shape != mov.shape:
        raise ValueError(f"size mismatch {ref.shape} != {mov.shape}")
    h, w = ref.shape
    mask = np.ones(ref.shape, bool)
    mask[int(face[1] * h):int(face[3] * h), int(face[0] * w):int(face[2] * w)] = False

    # Coarse on a quarter-size copy, then refine at full resolution, so a wide
    # search does not cost a wide search's time.
    q = 4
    small_ref, small_mov = ref[::q, ::q], mov[::q, ::q]
    _, cx, cy = _search(small_ref, small_mov, mask[::q, ::q], (0, 0),
                        max(1, radius // q), 1)
    err, dx, dy = _search(ref, mov, mask, (cx * q, cy * q), q, 1)
    return dx, dy, err


def align(ref_img: Image.Image, mov_img: Image.Image, **kw) -> tuple[Image.Image, int, int]:
    """mov shifted onto ref. Edges wrap, which only ever touches the backdrop."""
    dx, dy, _ = offset(ref_img, mov_img, **kw)
    if (dx, dy) == (0, 0):
        return mov_img, 0, 0
    a = np.asarray(mov_img.convert("RGBA"))
    a = np.roll(np.roll(a, dy, axis=0), dx, axis=1)
    return Image.fromarray(a), dx, dy
