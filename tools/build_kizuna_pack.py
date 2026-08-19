#!/usr/bin/env python3
"""Build the bundled Kizuna companion pack from its normalized 4x4 sheets.

Kizuna's generated artwork intentionally keeps a pure-white source background,
but its grey-blue irises are warmer than the original Claude Code sheets.  The
generic packer uses a cool-iris detector to register cells, so this dedicated
builder uses the much more stable face skin silhouette instead.  Everything is
still emitted in the normal M5Companion manifest/.m5a format; the firmware does
not know or care which builder produced the pack.

The builder also accepts ``--background``.  That makes scene/background changes
an offline pack operation rather than a firmware feature: build several packs
(e.g. ``kizuna-home`` and ``kizuna-desk``) and switch them with the existing pack
manager.  This keeps alpha compositing and image decoding off the ESP32.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m5a  # noqa: E402

SCREEN_W = 320
SCREEN_H = 240
SS = 2
GRID = 4

THEMES = {
    "light": {"base": (242, 243, 246), "grid": (231, 234, 239), "glow": 0.30, "erode": 3},
    "dark": {"base": (10, 13, 17), "grid": (15, 19, 25), "glow": 0.85, "erode": 5},
}


@dataclass
class Anchor:
    face: tuple[int, int, int, int]
    eye_center: tuple[float, float]
    face_width: float


@dataclass
class Recipe:
    name: str
    theme: str
    eye_center: tuple[int, int]
    eye_span: int
    eye_rect: tuple[int, int, int, int]
    mouth_rect: tuple[int, int, int, int]
    sheets: dict[str, str]
    eye_slots: list[list]
    visemes: list[list]
    expressions: dict
    gestures: dict

    @classmethod
    def load(cls, path: Path) -> "Recipe":
        data = json.loads(path.read_text())
        g = data["geometry"]
        recipe = cls(
            name=data.get("name", path.stem),
            theme=data.get("theme", "light"),
            eye_center=tuple(g["eye_center"]),
            eye_span=int(g["eye_span"]),
            eye_rect=tuple(g["eye_rect"]),
            mouth_rect=tuple(g["mouth_rect"]),
            sheets=dict(data["sheets"]),
            eye_slots=list(data["eye_slots"]),
            visemes=list(data["visemes"]),
            expressions=dict(data["expressions"]),
            gestures=dict(data.get("gestures", {})),
        )
        recipe.validate(path)
        return recipe

    def validate(self, path: Path) -> None:
        if self.theme not in THEMES:
            raise SystemExit(f"{path.name}: unknown theme {self.theme!r}")
        if [entry[0] for entry in self.eye_slots] != list(m5a.EYE_SLOTS):
            raise SystemExit(f"{path.name}: eye slot order must match tools/m5a.py")
        if len(self.visemes) != 8:
            raise SystemExit(f"{path.name}: expected 8 visemes, got {len(self.visemes)}")
        used = {entry[1] for entry in self.eye_slots + self.visemes}
        used |= {spec["cell"][0] for spec in self.expressions.values()}
        used |= {spec["sheet"] for spec in self.gestures.values()}
        missing = sorted(used - set(self.sheets))
        if missing:
            raise SystemExit(f"{path.name}: undeclared sheets: {', '.join(missing)}")
        for rect in (self.eye_rect, self.mouth_rect):
            x, y, w, h = rect
            if x < 0 or y < 0 or x + w > SCREEN_W or y + h > SCREEN_H:
                raise SystemExit(f"{path.name}: tile rectangle outside 320x240 screen")


def _largest_blob(mask: np.ndarray, min_area: int = 40):
    """Largest four-connected component as (area, bbox), without scipy/opencv."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    best = None
    ys, xs = np.nonzero(mask)
    for sy, sx in zip(ys, xs):
        if seen[sy, sx]:
            continue
        stack = [(int(sy), int(sx))]
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
        if len(pts) < min_area:
            continue
        if best is None or len(pts) > best[0]:
            a = np.asarray(pts)
            best = (
                len(pts),
                (int(a[:, 1].min()), int(a[:, 0].min()),
                 int(a[:, 1].max()), int(a[:, 0].max())),
            )
    return best


def find_anchor(cell: Image.Image) -> Anchor:
    """Register a cell from the face skin silhouette rather than iris colour."""
    raw = np.asarray(cell.convert("RGB"))
    a = raw.astype(np.int16)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    skin = (r > 195) & (r - g > 18) & (r - g < 70) & (g - b > 5) & (g - b < 45) & (b > 140)

    # The face is always in the upper 78% of a cell.  Removing the bottom keeps
    # bare legs or hands from winning on full/upper-body gesture artwork.
    skin[int(cell.height * 0.78):, :] = False
    blob = _largest_blob(skin)
    if blob is None:
        raise ValueError("no face-colour component found")
    _, (x0, y0, x1, y1) = blob
    fw = max(8.0, float(x1 - x0 + 1))
    fh = max(8.0, float(y1 - y0 + 1))

    # On this design the eye line sits at ~43% of the visible skin oval and the
    # inter-eye distance is ~60% of its width.  Only the centre is needed for
    # registration; actual eye artwork remains untouched.
    eye_center = ((x0 + x1) * 0.5, y0 + fh * 0.43)
    return Anchor((x0, y0, x1, y1), eye_center, fw)


def cut_out(cell: Image.Image, erode: int) -> Image.Image:
    """Flood-fill the connected white sheet background, preserving white clothes."""
    rgb = cell.convert("RGB")
    marker = (255, 0, 255)
    work = rgb.copy()
    w, h = work.size
    seeds = [(0, 0), (w - 1, 0), (w // 2, 0), (w // 4, 0), (3 * w // 4, 0),
             (0, h // 4), (w - 1, h // 4), (0, h // 2), (w - 1, h // 2)]
    for seed in seeds:
        if work.getpixel(seed) != marker:
            ImageDraw.floodfill(work, seed, marker, thresh=34)
    arr = np.asarray(work)
    bg = (arr[:, :, 0] == 255) & (arr[:, :, 1] == 0) & (arr[:, :, 2] == 255)
    alpha = np.where(bg, 0, 255).astype(np.uint8)
    mask = Image.fromarray(alpha, "L")
    if erode >= 3:
        mask = mask.filter(ImageFilter.MinFilter(erode | 1))
    mask = mask.filter(ImageFilter.GaussianBlur(0.7))
    out = rgb.convert("RGBA")
    out.putalpha(mask)
    return out


def make_background(accent, theme: str, custom: Image.Image | None = None) -> Image.Image:
    w, h = SCREEN_W * SS, SCREEN_H * SS
    if custom is not None:
        return ImageOps.fit(custom.convert("RGB"), (w, h), method=Image.LANCZOS).copy()

    t = THEMES[theme]
    bg = Image.new("RGB", (w, h), t["base"])
    d = ImageDraw.Draw(bg)
    step = max(16, w // 20)
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=t["grid"])
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=t["grid"])

    glow = Image.new("RGB", (w, h), t["base"])
    gd = ImageDraw.Draw(glow)
    cx, cy = w // 2, int(h * 0.40)
    for i in range(14, 0, -1):
        f = i / 14.0
        rad = int(w * 0.10 + w * 0.34 * f)
        if theme == "dark":
            colour = tuple(int(c * (1.0 - f) * 0.9) for c in accent)
        else:
            colour = tuple(int(base + (a - base) * (1.0 - f) * 0.22)
                           for a, base in zip(accent, t["base"]))
        gd.ellipse((cx - rad, cy - int(rad * 0.85), cx + rad, cy + int(rad * 0.85)), fill=colour)
    glow = glow.filter(ImageFilter.GaussianBlur(w * 0.03))
    bg = Image.blend(bg, glow, t["glow"])

    d = ImageDraw.Draw(bg)
    u = w / SCREEN_W
    d.line([(12 * u, 16 * u), (60 * u, 16 * u)], fill=(58, 92, 128), width=max(1, int(u)))
    d.ellipse((62 * u, 13 * u, 68 * u, 19 * u), fill=(224, 122, 48))
    d.line([(260 * u, 16 * u), (308 * u, 16 * u)], fill=(224, 122, 48), width=max(1, int(u)))
    d.ellipse((252 * u, 13 * u, 258 * u, 19 * u), fill=(58, 92, 128))
    return bg


def feather_mask(size, rect, radius: int, blur: float) -> Image.Image:
    x, y, w, h = rect
    inset = int(blur * 2)
    inset = min(inset, max(0, min(w, h) // 4))
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (x + inset, y + inset, x + w - 1 - inset, y + h - 1 - inset),
        radius=max(1, radius - inset // 2), fill=255,
    )
    return mask.filter(ImageFilter.GaussianBlur(blur))


def match_tone(source: Image.Image, target: Image.Image, rect, ring: int) -> Image.Image:
    x, y, w, h = rect
    ox0, oy0 = max(0, x - ring), max(0, y - ring)
    ox1, oy1 = min(source.width, x + w + ring), min(source.height, y + h + ring)
    s = np.asarray(source.crop((ox0, oy0, ox1, oy1))).astype(np.float32)
    t = np.asarray(target.crop((ox0, oy0, ox1, oy1))).astype(np.float32)
    outside = np.ones(s.shape[:2], dtype=bool)
    outside[y - oy0:y - oy0 + h, x - ox0:x - ox0 + w] = False
    if outside.sum() < 50:
        return source
    delta = np.clip(t[outside].mean(axis=0) - s[outside].mean(axis=0), -18, 18)
    arr = np.asarray(source).astype(np.float32) + delta
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGB")


def graft(base: Image.Image, variant: Image.Image, rect, radius=12, blur=5.0, ring=18) -> Image.Image:
    toned = match_tone(variant, base, rect, ring)
    mask = feather_mask(base.size, rect, radius, blur)
    out = base.copy()
    out.paste(toned, (0, 0), mask)
    return out


class CellLibrary:
    def __init__(self, source_dir: Path, recipe: Recipe, background: Image.Image | None):
        self.source_dir = source_dir
        self.recipe = recipe
        self.custom_background = background
        self.cells: dict[str, list[Image.Image]] = {}
        self.anchors: dict[str, list[Anchor]] = {}
        self.scale: dict[str, float] = {}
        self.frames: dict[tuple, Image.Image] = {}
        self.placed_layers: dict[tuple, Image.Image] = {}

    def _load(self, key: str) -> None:
        if key in self.cells:
            return
        path = self.source_dir / self.recipe.sheets[key]
        if not path.exists():
            raise SystemExit(f"sheet not found: {path}")
        image = Image.open(path).convert("RGB")
        if image.width % GRID or image.height % GRID:
            raise SystemExit(f"{path.name}: dimensions must divide evenly into 4x4")
        cw, ch = image.width // GRID, image.height // GRID
        cells, anchors = [], []
        for row in range(GRID):
            for col in range(GRID):
                cell = image.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
                try:
                    anchor = find_anchor(cell)
                except ValueError as exc:
                    raise SystemExit(f"{path.name} cell {row * GRID + col}: {exc}") from exc
                cells.append(cell)
                anchors.append(anchor)
        widths = sorted(a.face_width for a in anchors)
        median_width = widths[len(widths) // 2]
        # The Kizuna design's eye centres are ~60% of the face-skin width apart.
        self.scale[key] = self.recipe.eye_span / max(8.0, median_width * 0.60)
        self.cells[key] = cells
        self.anchors[key] = anchors

    def _compose_with(self, key: str, index: int, accent, anchor_index: int | None = None) -> Image.Image:
        self._load(key)
        if index < 0 or index >= 16:
            raise SystemExit(f"{key}: cell {index} outside 0..15")
        ref_index = index if anchor_index is None else anchor_index
        cache_key = (key, index, ref_index)
        placed = self.placed_layers.get(cache_key)
        if placed is None:
            anchor = self.anchors[key][ref_index]
            scale = self.scale[key]
            layer = cut_out(self.cells[key][index], THEMES[self.recipe.theme]["erode"])
            lw = max(1, int(round(layer.width * scale * SS)))
            lh = max(1, int(round(layer.height * scale * SS)))
            scaled = layer.resize((lw, lh), Image.LANCZOS)
            ox = int(round(self.recipe.eye_center[0] * SS - anchor.eye_center[0] * scale * SS))
            oy = int(round(self.recipe.eye_center[1] * SS - anchor.eye_center[1] * scale * SS))
            placed = Image.new("RGBA", (SCREEN_W * SS, SCREEN_H * SS), (0, 0, 0, 0))
            placed.paste(scaled, (ox, oy), scaled)
            self.placed_layers[cache_key] = placed
        frame = make_background(accent, self.recipe.theme, self.custom_background)
        frame.paste(placed.convert("RGB"), (0, 0), placed.getchannel("A"))
        return frame

    def frame(self, key: str, index: int, accent) -> Image.Image:
        cache_key = (key, index, tuple(accent))
        if cache_key not in self.frames:
            self.frames[cache_key] = self._compose_with(key, index, accent)
        return self.frames[cache_key]

    def clip_frames(self, key: str, indices: list[int], accent) -> list[Image.Image]:
        if not indices:
            return []
        self._load(key)
        # Use the first pose as the registration reference for the entire clip,
        # preserving subsequent nod/tilt/gesture displacement instead of pinning
        # each frame back to neutral.
        ref = int(indices[0])
        return [self._compose_with(key, int(i), accent, anchor_index=ref).resize(
                    (SCREEN_W, SCREEN_H), Image.LANCZOS) for i in indices]


def ss_rect(rect):
    return tuple(int(v * SS) for v in rect)


def crop_tile(image_ss: Image.Image, rect) -> Image.Image:
    x, y, w, h = ss_rect(rect)
    tile = image_ss.crop((x, y, x + w, y + h))
    return tile.resize((rect[2], rect[3]), Image.LANCZOS)


def write_preview(preview: dict[str, Image.Image], path: Path) -> None:
    cols = 5
    rows = (len(preview) + cols - 1) // cols
    card_w, card_h = SCREEN_W, SCREEN_H + 22
    out = Image.new("RGB", (cols * card_w, rows * card_h), (28, 30, 34))
    draw = ImageDraw.Draw(out)
    for i, (name, image) in enumerate(preview.items()):
        x, y = (i % cols) * card_w, (i // cols) * card_h
        out.paste(image, (x, y))
        draw.text((x + 5, y + SCREEN_H + 4), name, fill=(235, 235, 235))
    path.parent.mkdir(parents=True, exist_ok=True)
    out.save(path)
    print(f"preview -> {path}")


def build(args) -> int:
    recipe = Recipe.load(args.character)
    custom_bg = Image.open(args.background).convert("RGB") if args.background else None
    lib = CellLibrary(args.source, recipe, custom_bg)
    pack_name = args.name or recipe.name
    pack_dir = args.out / "companion" / "packs" / pack_name
    started = time.time()
    total_bytes = 0
    file_count = 0
    preview = {}

    names = args.only if args.only else list(recipe.expressions)
    for name in names:
        if name not in recipe.expressions:
            raise SystemExit(f"no such expression {name!r}")

    print(f"Kizuna builder: {len(names)} expressions -> {pack_dir}")
    for name in names:
        spec = recipe.expressions[name]
        sheet_key, cell_index = spec["cell"]
        accent = tuple(spec["accent"])
        base_ss = lib.frame(sheet_key, int(cell_index), accent)
        base = base_ss.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
        preview[name] = base

        if not args.preview:
            eye_tiles = []
            for _, key, index in recipe.eye_slots:
                variant = lib.frame(key, int(index), accent)
                merged = graft(base_ss, variant, ss_rect(recipe.eye_rect), radius=18, blur=7.0, ring=24)
                eye_tiles.append(crop_tile(merged, recipe.eye_rect))

            mouth_tiles = []
            for _, key, index in recipe.visemes:
                variant = lib.frame(key, int(index), accent)
                merged = graft(base_ss, variant, ss_rect(recipe.mouth_rect), radius=14, blur=6.0, ring=20)
                mouth_tiles.append(crop_tile(merged, recipe.mouth_rect))

            for sub, frames in (("base", [base]), ("eyes", eye_tiles), ("mouth", mouth_tiles)):
                total_bytes += m5a.write_clip(pack_dir / sub / f"{name}.m5a", frames)
                file_count += 1
        print(f"  {name}")

    clips = {}
    if not args.preview and not args.no_gestures:
        neutral_accent = tuple(recipe.expressions["neutral"]["accent"])
        for name, spec in recipe.gestures.items():
            frames = lib.clip_frames(spec["sheet"], [int(i) for i in spec["frames"]], neutral_accent)
            fps = max(1, min(5, int(spec.get("fps", 4))))
            total_bytes += m5a.write_clip(pack_dir / "clips" / f"{name}.m5a", frames, fps=fps)
            file_count += 1
            clips[name] = f"clips/{name}.m5a"
            print(f"  clip {name}: {len(frames)} frames @ {fps} fps")

    if args.preview:
        write_preview(preview, args.preview_path)
        return 0

    overlay_theme = args.background_theme or recipe.theme
    manifest = {
        "pack": pack_name,
        "character": recipe.name,
        "version": 2,
        "format": "rgb565be",
        "theme": overlay_theme,
        "screen": {"w": SCREEN_W, "h": SCREEN_H},
        "eye_center": {"x": recipe.eye_center[0], "y": recipe.eye_center[1]},
        "sway_rect": {"x": 0, "y": 0, "w": 0, "h": 0},
        "eye_rect": dict(zip("xywh", recipe.eye_rect)),
        "mouth_rect": dict(zip("xywh", recipe.mouth_rect)),
        "sway_frames": 1,
        "eye_slots": [entry[0] for entry in recipe.eye_slots],
        "viseme_frames": len(recipe.visemes),
        "visemes": [entry[0] for entry in recipe.visemes],
        "expressions": {
            name: {"base": f"base/{name}.m5a", "sway": "",
                   "eyes": f"eyes/{name}.m5a", "mouth": f"mouth/{name}.m5a"}
            for name in names
        },
        "clips": clips,
        "source_background": str(args.background) if args.background else "generated",
    }
    pack_dir.mkdir(parents=True, exist_ok=True)
    (pack_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n{file_count} .m5a files, {total_bytes / (1024 * 1024):.1f} MiB, "
          f"{time.time() - started:.1f}s")
    return 0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--character", type=Path, default=root / "assets/characters/kizuna.json")
    ap.add_argument("--source", type=Path, default=root / "assets/source")
    ap.add_argument("--out", type=Path, default=root / "build/sd")
    ap.add_argument("--name", default="kizuna")
    ap.add_argument("--background", type=Path, default=None,
                    help="optional scene image; use distinct --name values to keep several variants")
    ap.add_argument("--background-theme", choices=("light", "dark"), default=None,
                    help="status-bar theme when --background is used")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--no-gestures", action="store_true")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--preview-path", type=Path, default=root / "build/kizuna_preview.png")
    return build(ap.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
