"""Layered character rig: a blank base face plus overlay parts.

The earlier pipeline grafted rectangles out of one finished portrait onto
another and spent most of its effort hiding the seam - aligning by
cross-correlation, matching skin tone, feathering the mask. None of that is
needed here. The artist supplies faces with no eyes and no mouth, and the eyes,
mouths, brows and effects as separate pieces, so a pose is a composite rather
than a transplant.

What is needed instead is anchors: where on each base face the eyes and mouth
belong, and how wide. Those live in the character recipe.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def grid_cells(path, cols: int, rows: int, trim_label: float = 0.17,
               trim_edge: float = 0.02) -> list[Image.Image]:
    """Splits a sheet into cells by uniform division of its content box.

    These sheets have card outlines only a couple of levels away from the page
    background, so detecting the cards themselves is hopeless. The layout is
    perfectly regular, though, and the ink bounding box brackets it - so find
    that and divide.

    `trim_label` removes the caption strip at the bottom of every card.
    """
    image = Image.open(path).convert("RGB")
    gray = np.asarray(image.convert("L")).astype(np.int16)
    ink = gray < 200

    row_profile = ink.mean(axis=1)
    ys = np.nonzero(row_profile > 0.004)[0]
    body = ys[ys > 150]  # skip the title band
    if body.size == 0:
        raise SystemExit(f"{path}: no content found")
    y0, y1 = int(body.min()), int(body.max())

    xs = np.nonzero(ink[y0:y1].mean(axis=0) > 0.002)[0]
    x0, x1 = int(xs.min()), int(xs.max())

    cells = []
    for r in range(rows):
        for c in range(cols):
            cx0 = x0 + (x1 - x0) * c // cols
            cx1 = x0 + (x1 - x0) * (c + 1) // cols
            cy0 = y0 + (y1 - y0) * r // rows
            cy1 = y0 + (y1 - y0) * (r + 1) // rows
            w, h = cx1 - cx0, cy1 - cy0
            cells.append(image.crop((
                cx0 + int(w * trim_edge), cy0 + int(h * trim_edge),
                cx1 - int(w * trim_edge), cy1 - int(h * trim_label))))
    return cells


def cut_part(cell: Image.Image, thresh: int = 26, erode: int = 0) -> Image.Image:
    """Removes the card background, keeping enclosed whites.

    The white of an eye and the teeth in a smile are as pale as the page, so a
    threshold would delete them. Flooding inwards from the border cannot reach
    them - the lash line and the lips enclose them completely.
    """
    rgb = cell.convert("RGB")
    marker = (255, 0, 255)
    work = rgb.copy()
    w, h = work.size
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
             (w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2)]
    for seed in seeds:
        if work.getpixel(seed) != marker:
            ImageDraw.floodfill(work, seed, marker, thresh=thresh)

    arr = np.asarray(work)
    bg = (arr[:, :, 0] == 255) & (arr[:, :, 1] == 0) & (arr[:, :, 2] == 255)
    mask = Image.fromarray(np.where(bg, 0, 255).astype(np.uint8), "L")
    if erode >= 3:
        mask = mask.filter(ImageFilter.MinFilter(erode | 1))

    out = rgb.convert("RGBA")
    out.putalpha(mask.filter(ImageFilter.GaussianBlur(0.6)))
    return out


def trim(part: Image.Image) -> Image.Image:
    """Crops an RGBA part to its own content."""
    alpha = np.asarray(part)[:, :, 3]
    ys, xs = np.nonzero(alpha > 40)
    if xs.size == 0:
        return part
    return part.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))


def is_skin(arr: np.ndarray) -> np.ndarray:
    r = arr[:, :, 0].astype(np.int32)
    g = arr[:, :, 1].astype(np.int32)
    b = arr[:, :, 2].astype(np.int32)
    return (r > 195) & (r - g > 18) & (r - g < 75) & (g - b > 4) & (g - b < 50) & (b > 135)


@dataclass
class FaceMetrics:
    """The skin region of a blank base face, in cell pixels."""
    x0: int
    y0: int
    x1: int
    y1: int

    @property
    def width(self) -> int:
        return self.x1 - self.x0

    @property
    def height(self) -> int:
        return self.y1 - self.y0

    def point(self, fx: float, fy: float) -> tuple[float, float]:
        return (self.x0 + self.width * fx, self.y0 + self.height * fy)


def face_metrics(cell: Image.Image) -> FaceMetrics:
    """The face, from hairline to chin - deliberately not the whole skin area.

    A plain bounding box of every skin pixel runs down the neck to the collar,
    which makes "40% of the way down" land on the nose in one pose and the lip
    in another. The jaw is the fix: skin width peaks at the cheekbones and
    collapses at the chin before widening again for the neck, so the first
    sharp narrowing below the peak is where the face ends.
    """
    arr = np.asarray(cell.convert("RGB"))
    skin = is_skin(arr)
    widths = skin.sum(axis=1)
    if widths.max() < 20:
        raise SystemExit("no facial skin found in this cell")

    peak_row = int(np.argmax(widths))
    peak = widths[peak_row]

    top = 0
    for y in range(peak_row, -1, -1):
        if widths[y] < peak * 0.25:
            top = y
            break

    chin = len(widths) - 1
    for y in range(peak_row, len(widths)):
        if widths[y] < peak * 0.45:
            chin = y
            break

    band = skin[top:chin + 1]
    xs = np.nonzero(band.any(axis=0))[0]
    if xs.size == 0:
        raise SystemExit("face band is empty")
    return FaceMetrics(int(xs.min()), int(top), int(xs.max()), int(chin))


def place(canvas: Image.Image, part: Image.Image, center, width: float,
          rotate: float = 0.0) -> Image.Image:
    """Scales `part` to `width`, centres it on `center`, and composites."""
    piece = trim(part)
    if piece.width == 0:
        return canvas
    scale = width / piece.width
    size = (max(1, int(round(piece.width * scale))), max(1, int(round(piece.height * scale))))
    piece = piece.resize(size, Image.LANCZOS)
    if rotate:
        piece = piece.rotate(rotate, resample=Image.BICUBIC, expand=True)
    out = canvas.copy()
    out.alpha_composite(piece, (int(round(center[0] - piece.width / 2)),
                                int(round(center[1] - piece.height / 2))))
    return out
