"""Generic cell finder for the M5Stack companion asset sheets.

Every sheet is a light page with dark character art arranged in rows. Instead
of hard-coding pixel offsets per sheet - which would break the moment the
artist regenerates one - rows and columns are found by projecting the dark
pixels and looking for bands.
"""

from __future__ import annotations

import numpy as np
from PIL import Image


def _bands(profile: np.ndarray, threshold: float, min_len: int, gap: int = 6):
    """Contiguous runs above `threshold`, merged across short gaps."""
    hot = profile > threshold
    runs = []
    start = None
    misses = 0
    for i, v in enumerate(hot):
        if v:
            if start is None:
                start = i
            misses = 0
        elif start is not None:
            misses += 1
            if misses > gap:
                if i - misses - start >= min_len:
                    runs.append((start, i - misses))
                start = None
                misses = 0
    if start is not None and len(hot) - start >= min_len:
        runs.append((start, len(hot) - 1))
    return runs


def find_cells(image: Image.Image, dark: int = 165, row_min: int = 60, col_min: int = 60,
               min_cell_height: int = 120):
    """Returns a list of rows, each a list of (x0, y0, x1, y1) cell boxes.

    Rows shorter than `min_cell_height` are dropped: every sheet carries a
    title band whose logo and lettering project exactly like a row of cells.
    """
    gray = np.asarray(image.convert("L")).astype(np.float32)
    h, w = gray.shape
    mask = gray < dark

    row_profile = mask.mean(axis=1)
    rows = _bands(row_profile, 0.045, row_min, gap=10)

    out = []
    for (y0, y1) in rows:
        band = mask[y0:y1 + 1, :]
        col_profile = band.mean(axis=0)
        cols = _bands(col_profile, 0.05, col_min, gap=12)
        if y1 - y0 < min_cell_height:
            continue
        out.append([(x0, y0, x1, y1) for (x0, x1) in cols])
    return out


def describe(path: str):
    im = Image.open(path)
    rows = find_cells(im)
    print(f"{path}: {im.size}")
    for i, row in enumerate(rows):
        print(f"  row {i}: {len(row)} cells  y={row[0][1]}..{row[0][3]}  "
              f"x={[c[0] for c in row]}")
    return rows


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        describe(p)
