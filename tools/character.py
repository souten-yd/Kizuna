"""Turns one asset-sheet cell into an aligned 320x240 companion frame.

Every sheet was generated separately, so nothing is registered: the head sits
at a different place and size in each cell. Alignment happens here, on the
eyes, because the eyes are what the viewer's attention locks onto and because
the renderer later swaps eye and mouth rectangles between frames - which only
works if those rectangles cover the same anatomy everywhere.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SCREEN_W, SCREEN_H = 320, 240
SS = 2  # supersampling while compositing


@dataclass
class Landmarks:
    face: tuple          # x0, y0, x1, y1 of the skin region
    left: tuple          # bounding box of the left eye
    right: tuple
    eye_center: tuple
    inter: float
    mouth: tuple
    confident: bool      # False when the eyes had to be inferred


def is_skin(arr: np.ndarray) -> np.ndarray:
    r = arr[:, :, 0].astype(np.int32)
    g = arr[:, :, 1].astype(np.int32)
    b = arr[:, :, 2].astype(np.int32)
    return (r > 195) & (r - g > 18) & (r - g < 70) & (g - b > 5) & (g - b < 45) & (b > 140)


def largest_blobs(mask: np.ndarray, count: int, min_area: int = 60):
    """Connected components, biggest first. There is no scipy in this toolchain."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    blobs = []
    ys, xs = np.nonzero(mask)
    for sy, sx in zip(ys, xs):
        if seen[sy, sx]:
            continue
        stack = [(sy, sx)]
        seen[sy, sx] = True
        pts = []
        while stack:
            y, x = stack.pop()
            pts.append((y, x))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        if len(pts) >= min_area:
            a = np.array(pts)
            blobs.append((len(pts), (int(a[:, 1].min()), int(a[:, 0].min()),
                                     int(a[:, 1].max()), int(a[:, 0].max()))))
    blobs.sort(key=lambda b: -b[0])
    return [b[1] for b in blobs[:count]]


def box_center(box):
    return ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)


def find_landmarks(cell: Image.Image, fallback=None):
    """Locates both eyes and the mouth in one portrait cell.

    The irises are the only cool-toned thing on the character: skin runs an
    R-B of about +65 and the brown hair about +15, while the iris is genuinely
    blue-grey. That single sign flip separates eyes from hair, lashes and
    shadow far more reliably than any brightness threshold.
    """
    rgb = cell.convert("RGB")
    raw = np.asarray(rgb)
    arr = raw.astype(np.int32)
    R, G, B = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
    luma = (R * 299 + G * 587 + B * 114) // 1000

    skin = is_skin(raw)
    ys, xs = np.nonzero(skin)
    if xs.size < 200:
        return None
    fx0, fx1 = int(np.percentile(xs, 3)), int(np.percentile(xs, 97))
    fy0, fy1 = int(np.percentile(ys, 3)), int(np.percentile(ys, 97))
    fh, fcx = fy1 - fy0, (fx0 + fx1) / 2

    candidate = (B - R > 6) & (luma > 35) & (luma < 200)
    candidate[: fy0 + int(fh * 0.02), :] = False
    candidate[fy0 + int(fh * 0.48):, :] = False
    candidate[:, : fx0 - 6] = False
    candidate[:, fx1 + 6:] = False

    # Dilate so the sclera slivers on either side of an iris merge into one blob.
    merged = Image.fromarray((candidate * 255).astype(np.uint8), "L")
    merged = merged.filter(ImageFilter.MaxFilter(7))
    blobs = largest_blobs(np.asarray(merged) > 127, 6, min_area=100)

    lefts = [b for b in blobs if box_center(b)[0] < fcx]
    rights = [b for b in blobs if box_center(b)[0] >= fcx]
    confident = bool(lefts and rights)

    def mirror(box):
        return (int(2 * fcx - box[2]), box[1], int(2 * fcx - box[0]), box[3])

    if lefts and not rights:
        rights = [mirror(lefts[0])]
    elif rights and not lefts:
        lefts = [mirror(rights[0])]
    elif not lefts and not rights:
        # Both eyes shut. Fall back to the geometry measured on the cells where
        # they were open - the blink and sleepy frames are exactly this case.
        if fallback is None:
            return None
        inter_f, y_ratio, hw, hh = fallback
        cy = fy0 + fh * y_ratio
        lefts = [(int(fcx - inter_f / 2 - hw), int(cy - hh),
                  int(fcx - inter_f / 2 + hw), int(cy + hh))]
        rights = [(int(fcx + inter_f / 2 - hw), int(cy - hh),
                   int(fcx + inter_f / 2 + hw), int(cy + hh))]

    def shrink(box, m=3):
        return (box[0] + m, box[1] + m, box[2] - m, box[3] - m)

    left, right = shrink(lefts[0]), shrink(rights[0])
    lc, rc = box_center(left), box_center(right)
    center = ((lc[0] + rc[0]) / 2, (lc[1] + rc[1]) / 2)
    inter = max(8.0, rc[0] - lc[0])

    # Mouth: reddish and dark, in a band below the eyes but above the choker,
    # which is itself both dark and warm and would otherwise win.
    warm = (luma < 175) & (R - G > 22)
    warm[: int(center[1] + inter * 0.32), :] = False
    warm[int(center[1] + inter * 0.82):, :] = False
    warm[:, : int(center[0] - inter * 0.32)] = False
    warm[:, int(center[0] + inter * 0.32):] = False

    counts = warm.sum(axis=1).astype(np.float32)
    if counts.max() >= 6:
        smooth = np.convolve(counts, np.ones(5, dtype=np.float32) / 5.0, mode="same")
        my = int(np.argmax(smooth))
        _, mxs = np.nonzero(warm[max(0, my - 3):my + 4, :])
        mx = float(mxs.mean()) if mxs.size else center[0]
        limit = inter * 0.15
        mx = min(max(mx, center[0] - limit), center[0] + limit)
        mouth = (mx, float(my))
    else:
        mouth = (center[0], center[1] + inter * 0.62)

    return Landmarks((fx0, fy0, fx1, fy1), left, right, center, inter, mouth, confident)


def cut_out(cell: Image.Image, erode: int = 3) -> Image.Image:
    """Removes the near-white sheet background while keeping the white shirt.

    A global "delete everything pale" threshold would eat the T-shirt, so the
    background is found by flood filling inwards from the border instead.

    `erode` trims the sheet's anti-aliased edge pixels, which are lighter than
    the fill threshold and survive as a white fringe. How much you need
    depends entirely on the backdrop: against a light one the fringe is
    invisible and eroding only chews into the artwork, so the light theme
    passes a small value here and the dark theme a large one.
    """
    rgb = cell.convert("RGB")
    marker = (255, 0, 255)
    work = rgb.copy()
    w, h = work.size
    # Seed only along the top and the upper sides. On the newer sheets the
    # character is cropped by the bottom edge, so a seed down there would flood
    # straight through the white T-shirt and punch holes in the chest.
    seeds = [(0, 0), (w - 1, 0), (w // 2, 0), (w // 4, 0), (3 * w // 4, 0),
             (0, h // 8), (w - 1, h // 8), (0, h // 3), (w - 1, h // 3)]
    for seed in seeds:
        if work.getpixel(seed) != marker:
            ImageDraw.floodfill(work, seed, marker, thresh=30)

    arr = np.asarray(work)
    bg = (arr[:, :, 0] == 255) & (arr[:, :, 1] == 0) & (arr[:, :, 2] == 255)
    alpha = np.where(bg, 0, 255).astype(np.uint8)

    out = rgb.convert("RGBA")
    mask = Image.fromarray(alpha, "L")
    if erode >= 3:
        mask = mask.filter(ImageFilter.MinFilter(erode | 1))
    out.putalpha(mask.filter(ImageFilter.GaussianBlur(0.8)))
    return out


def compose(cell: Image.Image, marks: Landmarks, scale: float, background: Image.Image,
            eye_center=(160, 100), dy: float = 0.0, dx: float = 0.0,
            erode: int = 3) -> Image.Image:
    """Places one cut-out cell on the backdrop with its eyes on the anchor."""
    layer = cut_out(cell, erode)
    lw = max(1, int(round(layer.width * scale * SS)))
    lh = max(1, int(round(layer.height * scale * SS)))
    scaled = layer.resize((lw, lh), Image.LANCZOS)
    ox = int(round((eye_center[0] + dx) * SS - marks.eye_center[0] * scale * SS))
    oy = int(round((eye_center[1] + dy) * SS - marks.eye_center[1] * scale * SS))
    frame = background.copy()
    frame.paste(scaled, (ox, oy), scaled)
    return frame


def feather_mask(size, rect, radius: int, blur: float) -> Image.Image:
    """A rounded, blurred rectangle used to blend one cell's face onto another.

    The shape is inset before blurring so the mask reaches zero *inside* the
    rectangle. Drawn flush to the edge instead, a blur leaves the mask at
    roughly half strength along the border - and since the tile is cropped to
    exactly that rectangle, every edge pixel would ship as a 50/50 mix of two
    faces. That is a visible seam on all four sides of the eyes and mouth.
    """
    x, y, w, h = rect
    inset = int(blur * 2)
    if w - 2 * inset < 4 or h - 2 * inset < 4:
        inset = max(0, min(w, h) // 4)

    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (x + inset, y + inset, x + w - 1 - inset, y + h - 1 - inset),
        radius=max(1, radius - inset // 2), fill=255)
    return mask.filter(ImageFilter.GaussianBlur(blur))


def match_tone(source: Image.Image, target: Image.Image, rect, ring: int) -> Image.Image:
    """Shifts `source` so the skin just outside `rect` matches `target`.

    The sheets were generated in separate passes and their skin tones drift by
    a few levels. Left alone that drift shows up as a visible rectangle around
    every swapped mouth, which is exactly the artefact this whole pipeline
    exists to avoid.
    """
    x, y, w, h = rect
    ox0, oy0 = max(0, x - ring), max(0, y - ring)
    ox1, oy1 = min(source.size[0], x + w + ring), min(source.size[1], y + h + ring)

    s = np.asarray(source.crop((ox0, oy0, ox1, oy1))).astype(np.float32)
    t = np.asarray(target.crop((ox0, oy0, ox1, oy1))).astype(np.float32)

    inner = np.ones(s.shape[:2], dtype=bool)
    inner[y - oy0:y - oy0 + h, x - ox0:x - ox0 + w] = False
    if inner.sum() < 50:
        return source

    delta = t[inner].mean(axis=0) - s[inner].mean(axis=0)
    delta = np.clip(delta, -24, 24)
    arr = np.asarray(source).astype(np.float32) + delta
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGB")


def graft(base: Image.Image, variant: Image.Image, rect, radius: int = 10,
          blur: float = 5.0, ring: int = 12, align: bool = True,
          search: int = 20) -> Image.Image:
    """Blends `variant`'s rect into `base`, aligned, tone-matched and feathered.

    Alignment is not optional in practice. The sheets were generated in
    separate passes, so the head lands a few pixels - sometimes a few dozen -
    apart between them, and a blink pasted at the wrong offset reads as the
    whole face jumping rather than as an eyelid closing.
    """
    if align:
        dx, dy = estimate_offset(base, variant, rect, search)
        variant = shifted(variant, dx, dy)
    toned = match_tone(variant, base, rect, ring)
    mask = feather_mask(base.size, rect, radius, blur)
    out = base.copy()
    out.paste(toned, (0, 0), mask)
    return out


def silhouette_anchor(cell: Image.Image):
    """Top-centre of the character's outline.

    Unlike the eyes, the silhouette is present in every cell - open, blinking
    or fast asleep - so it is the one landmark that can tie a closed-eye frame
    back into the same reference frame as its open-eyed neighbours.
    """
    alpha = np.asarray(cut_out(cell, erode=3))[:, :, 3]
    ys, xs = np.nonzero(alpha > 64)
    if xs.size < 200:
        return None
    # Percentiles, not min/max: a stray anti-aliased pixel should not define
    # where the head starts.
    return (float(np.percentile(xs, 50)), float(np.percentile(ys, 1)))


def estimate_offset(base: Image.Image, variant: Image.Image, rect, search: int = 20,
                    downsample: int = 4):
    """How far `variant` must move to line up with `base` around `rect`.

    Matching is done on the ring *around* the rectangle - the cheek, jaw and
    hair - never inside it. Inside is precisely where the two frames are
    supposed to differ, so including it would score a correct alignment as
    a bad one.
    """
    x, y, w, h = rect
    pad = max(w, h) // 2
    bx0, by0 = max(0, x - pad), max(0, y - pad)
    bx1, by1 = min(base.size[0], x + w + pad), min(base.size[1], y + h + pad)

    b = np.asarray(base.crop((bx0, by0, bx1, by1)).convert("L"),
                   dtype=np.float32)[::downsample, ::downsample]
    ring = np.ones(b.shape, dtype=bool)
    iy0, ix0 = (y - by0) // downsample, (x - bx0) // downsample
    ring[iy0:iy0 + h // downsample, ix0:ix0 + w // downsample] = False
    if ring.sum() < 64:
        return (0, 0)

    def score(dx, dy):
        cx0, cy0 = bx0 + dx, by0 + dy
        if cx0 < 0 or cy0 < 0 or cx0 + (bx1 - bx0) > variant.size[0] \
                or cy0 + (by1 - by0) > variant.size[1]:
            return float("inf")
        v = np.asarray(variant.crop((cx0, cy0, cx0 + (bx1 - bx0), cy0 + (by1 - by0)))
                       .convert("L"), dtype=np.float32)[::downsample, ::downsample]
        if v.shape != b.shape:
            return float("inf")
        return float(np.abs(b[ring] - v[ring]).mean())

    # Coarse then fine: a full search at single-pixel resolution over +-20 px
    # would be 1681 evaluations per tile, and this runs thousands of times.
    best = (0, 0)
    best_score = score(0, 0)
    for step in (downsample, 1):
        cx, cy = best
        span = search if step == downsample else step * 2
        for dy in range(cy - span, cy + span + 1, step):
            for dx in range(cx - span, cx + span + 1, step):
                sc = score(dx, dy)
                if sc < best_score:
                    best_score, best = sc, (dx, dy)
    return best


def shifted(image: Image.Image, dx: int, dy: int) -> Image.Image:
    if dx == 0 and dy == 0:
        return image
    out = Image.new("RGB", image.size)
    out.paste(image, (-dx, -dy))
    # Clamp the vacated edges so the graft never samples black.
    if dx > 0:
        out.paste(image.crop((image.width - 1, 0, image.width, image.height))
                  .resize((dx, image.height)), (image.width - dx, 0))
    elif dx < 0:
        out.paste(image.crop((0, 0, 1, image.height)).resize((-dx, image.height)), (0, 0))
    if dy > 0:
        out.paste(out.crop((0, image.height - dy - 1, image.width, image.height - dy))
                  .resize((image.width, dy)), (0, image.height - dy))
    elif dy < 0:
        out.paste(out.crop((0, -dy, image.width, -dy + 1)).resize((image.width, -dy)), (0, 0))
    return out


def crop_tile(frame: Image.Image, rect, scale: int = SS) -> Image.Image:
    x, y, w, h = rect
    tile = frame.crop((x * scale, y * scale, (x + w) * scale, (y + h) * scale))
    return tile.resize((w, h), Image.LANCZOS) if scale != 1 else tile
