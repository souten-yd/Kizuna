#!/usr/bin/env python3
"""Builds an M5Companion character pack from the M5Stack companion asset sheets.

The firmware decodes nothing at runtime, so all of the work happens here: cells
are cut off their sheets, aligned on a common eye line, composited over a
generated backdrop, and sliced into the small rectangles the renderer blits.

Nothing is synthesised. Every blink stage, viseme and gaze direction is a real
drawn cell, grafted onto each expression's face with a feathered, tone-matched
mask so the seam does not show. That is the whole reason for putting a 64 GB
card behind an ESP32: storage is free, and CPU cycles are not.

    python tools/pack_assets.py --out build/sd
    python tools/pack_assets.py --preview
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))

import character as ch  # noqa: E402
import m5a  # noqa: E402
import sheet as sh  # noqa: E402

SCREEN_W, SCREEN_H = ch.SCREEN_W, ch.SCREEN_H
SS = ch.SS

# --------------------------------------------------------------------------
# Character recipes
#
# Which sheet cell becomes which blink stage, viseme or expression is data,
# not code: it lives in assets/characters/<name>.json. Adding artwork, or a
# whole second character, means writing another recipe - never editing this
# file. Packs land side by side on the card and the firmware picks one by name.
# --------------------------------------------------------------------------
SCREEN_W, SCREEN_H = ch.SCREEN_W, ch.SCREEN_H
SS = ch.SS

# Backdrop themes.
#
# "light" matches the sheets the character was drawn on, which is why it is the
# default: the artwork's own soft outline blends into it, so no edge trimming
# is needed and none of the hair gets chewed off. "dark" looks better on a desk
# at night and draws less current, at the cost of a visible rim where the
# cut-out ends - hence the heavier erode.
THEMES = {
    "light": {"base": (242, 243, 246), "grid": (231, 234, 239), "glow": 0.30,
              "erode": 3, "overlay": "light"},
    "dark": {"base": (10, 13, 17), "grid": (15, 19, 25), "glow": 0.85,
             "erode": 9, "overlay": "dark"},
}


@dataclass
class Recipe:
    name: str
    theme: str
    eye_center: tuple
    eye_span: int
    eye_rect: tuple
    mouth_rect: tuple
    sheets: dict
    eye_slots: list      # [(slot_name, sheet_key, cell_index), ...]
    visemes: list        # [(viseme_name, sheet_key, cell_index), ...]
    expressions: dict    # name -> {"cell": (sheet, idx), "accent": (r, g, b)}
    gestures: dict       # name -> {"sheet": key, "frames": [...], "fps": n}
    path: Path

    @staticmethod
    def load(path: Path) -> "Recipe":
        data = json.loads(path.read_text())
        g = data["geometry"]

        theme = data.get("theme", "light")
        if theme not in THEMES:
            raise SystemExit(f"{path.name}: unknown theme {theme!r}")

        eye_slots = [(n, s, i) for n, s, i in data["eye_slots"]]
        if len(eye_slots) != len(m5a.EYE_SLOTS):
            raise SystemExit(f"{path.name}: expected {len(m5a.EYE_SLOTS)} eye slots, "
                             f"got {len(eye_slots)}")
        for got, want in zip((n for n, _, _ in eye_slots), m5a.EYE_SLOTS):
            if got != want:
                raise SystemExit(f"{path.name}: eye slot order must match the firmware "
                                 f"({want} expected, {got} found)")

        expressions = {
            name: {"cell": tuple(spec["cell"]), "accent": tuple(spec["accent"])}
            for name, spec in data["expressions"].items()
        }

        recipe = Recipe(
            name=data.get("name", path.stem),
            theme=theme,
            eye_center=tuple(g["eye_center"]),
            eye_span=int(g["eye_span"]),
            eye_rect=tuple(g["eye_rect"]),
            mouth_rect=tuple(g["mouth_rect"]),
            sheets=data["sheets"],
            eye_slots=eye_slots,
            visemes=[(n, s, i) for n, s, i in data["visemes"]],
            expressions=expressions,
            gestures=data.get("gestures", {}),
            path=path,
        )
        recipe.validate()
        return recipe

    def validate(self):
        """Catches a typo in a recipe here rather than 200 tiles later."""
        used = {s for _, s, _ in self.eye_slots} | {s for _, s, _ in self.visemes}
        used |= {spec["cell"][0] for spec in self.expressions.values()}
        used |= {spec["sheet"] for spec in self.gestures.values()}
        missing = sorted(used - set(self.sheets))
        if missing:
            raise SystemExit(f"{self.path.name}: sheets not declared: {', '.join(missing)}")

        for x, y, w, h in (self.eye_rect, self.mouth_rect):
            if x < 0 or y < 0 or x + w > SCREEN_W or y + h > SCREEN_H:
                raise SystemExit(f"{self.path.name}: a tile rect falls outside the screen")


# --------------------------------------------------------------------------
def make_background(accent, w, h, theme) -> Image.Image:
    """A technical panel with a soft accent glow behind the character."""
    t = THEMES[theme]
    bg = Image.new("RGB", (w, h), t["base"])
    d = ImageDraw.Draw(bg)
    step = max(8, w // 20)
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=t["grid"])
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=t["grid"])

    glow = Image.new("RGB", (w, h), t["base"])
    gd = ImageDraw.Draw(glow)
    cx, cy = w // 2, int(h * 0.40)
    for i in range(14, 0, -1):
        f = i / 14.0
        r = int(w * 0.10 + w * 0.34 * f)
        if theme == "dark":
            colour = tuple(int(c * (1.0 - f) * 0.9) for c in accent)
        else:
            # Tint towards the accent instead of away from black.
            colour = tuple(int(b + (a - b) * (1.0 - f) * 0.22)
                           for a, b in zip(accent, t["base"]))
        gd.ellipse((cx - r, cy - int(r * 0.85), cx + r, cy + int(r * 0.85)), fill=colour)
    glow = glow.filter(ImageFilter.GaussianBlur(w * 0.03))
    bg = Image.blend(bg, glow, t["glow"])

    d = ImageDraw.Draw(bg)
    u = w / SCREEN_W
    d.line([(12 * u, 16 * u), (60 * u, 16 * u)], fill=(58, 92, 128), width=max(1, int(u)))
    d.ellipse((62 * u, 13 * u, 68 * u, 19 * u), fill=(224, 122, 48))
    d.line([(260 * u, 16 * u), (308 * u, 16 * u)], fill=(224, 122, 48), width=max(1, int(u)))
    d.ellipse((252 * u, 13 * u, 258 * u, 19 * u), fill=(58, 92, 128))
    return bg


class CellLibrary:
    """Loads sheets lazily and hands out aligned, composited frames."""

    def __init__(self, source_dir: Path, recipe: Recipe):
        self.dir = source_dir
        self.recipe = recipe
        self.theme = recipe.theme
        self._cells: dict[str, list[Image.Image]] = {}
        self._marks: dict[str, list] = {}
        self._scale: dict[str, float] = {}
        self._frames: dict[tuple, Image.Image] = {}

    def _load(self, key: str):
        if key in self._cells:
            return
        if key not in self.recipe.sheets:
            raise SystemExit(f"recipe does not declare sheet {key!r}")
        path = self.dir / self.recipe.sheets[key]
        if not path.exists():
            raise SystemExit(f"sheet not found: {path}")
        image = Image.open(path).convert("RGB")
        rows = [r for r in sh.find_cells(image) if len(r) == 4]
        cells = [image.crop((x0 - 8, y0 - 8, x1 + 8, y1 + 8))
                 for row in rows for (x0, y0, x1, y1) in row]
        if len(cells) != 16:
            raise SystemExit(f"{path.name}: expected 16 cells, found {len(cells)}")

        marks = [ch.find_landmarks(c) for c in cells]
        confident = [m for m in marks if m is not None and m.confident]
        if not confident:
            raise SystemExit(f"{path.name}: no cell yielded a confident eye detection")

        def median(values):
            v = sorted(values)
            return v[len(v) // 2]

        # Frames with both eyes shut have no iris to find. Re-run them with the
        # geometry measured on the frames that do - the blink and sleepy cells
        # are exactly this case, and they are the ones that matter most.
        fallback = (
            median(m.inter for m in confident),
            median((m.eye_center[1] - m.face[1]) / max(1, m.face[3] - m.face[1])
                   for m in confident),
            median((m.left[2] - m.left[0]) / 2 for m in confident),
            median((m.left[3] - m.left[1]) / 2 for m in confident),
        )
        for i, m in enumerate(marks):
            if m is None or not m.confident:
                redone = ch.find_landmarks(cells[i], fallback)
                if redone is not None:
                    marks[i] = redone
        if any(m is None for m in marks):
            raise SystemExit(f"{path.name}: landmark detection failed on some cells")

        # Trusting the detector's own confidence is not enough: on a closed
        # eye it sometimes locks onto a brow or a shadow, reports success, and
        # puts the eyes tens of pixels from where they are. The silhouette
        # cannot lie that way, so every cell is checked against it and the
        # outliers - confident or not - are replaced.
        anchors = [ch.silhouette_anchor(c) for c in cells]
        usable = [i for i, a in enumerate(anchors) if a is not None]
        if usable:
            deltas = [(marks[i].eye_center[0] - anchors[i][0],
                       marks[i].eye_center[1] - anchors[i][1]) for i in usable]
            dx, dy = median(d[0] for d in deltas), median(d[1] for d in deltas)

            # Re-derive the median from the cells that agree with it, so a
            # cluster of bad cells cannot drag the reference off.
            inliers = [d for d in deltas if abs(d[0] - dx) <= 8 and abs(d[1] - dy) <= 8]
            if len(inliers) >= 3:
                dx = median(d[0] for d in inliers)
                dy = median(d[1] for d in inliers)

            corrected = []
            for i in usable:
                predicted = (anchors[i][0] + dx, anchors[i][1] + dy)
                actual = marks[i].eye_center
                if abs(actual[0] - predicted[0]) > 8 or abs(actual[1] - predicted[1]) > 8:
                    marks[i] = replace(marks[i], eye_center=predicted)
                    corrected.append(i)
            if corrected:
                print(f"  {path.name}: re-anchored cells {corrected} from the silhouette")

        # One scale for the whole sheet. Per-cell scaling would amplify the
        # noise in a single bad detection into a visibly larger head.
        self._cells[key] = cells
        self._marks[key] = marks
        self._scale[key] = self.recipe.eye_span / median(m.inter for m in confident)

    def frame(self, key: str, index: int, accent) -> Image.Image:
        """A composited 320x240 frame at supersampled resolution."""
        cache_key = (key, index, accent)
        if cache_key in self._frames:
            return self._frames[cache_key]
        self._load(key)
        background = make_background(accent, SCREEN_W * SS, SCREEN_H * SS, self.theme)
        frame = ch.compose(self._cells[key][index], self._marks[key][index],
                           self._scale[key], background, self.recipe.eye_center,
                           erode=THEMES[self.theme]["erode"])
        self._frames[cache_key] = frame
        return frame

    def clip_frames(self, key: str, indices: list, accent) -> list:
        """Composites a gesture clip, preserving the head motion.

        Tiles pin every cell's eyes to the same screen position, which is what
        keeps a blink from moving the face. A nod is the opposite case: the
        head moving *is* the animation, so the whole clip is placed once from
        its first frame and every later frame inherits that placement.
        """
        self._load(key)
        cells, marks, scale = self._cells[key], self._marks[key], self._scale[key]
        ref = marks[indices[0]]
        background = make_background(accent, SCREEN_W * SS, SCREEN_H * SS, self.theme)
        erode = THEMES[self.theme]["erode"]

        out = []
        for i in indices:
            layer = ch.cut_out(cells[i], erode)
            lw = max(1, int(round(layer.width * scale * SS)))
            lh = max(1, int(round(layer.height * scale * SS)))
            scaled = layer.resize((lw, lh), Image.LANCZOS)
            ox = int(round(self.recipe.eye_center[0] * SS - ref.eye_center[0] * scale * SS))
            oy = int(round(self.recipe.eye_center[1] * SS - ref.eye_center[1] * scale * SS))
            frame = background.copy()
            frame.paste(scaled, (ox, oy), scaled)
            out.append(frame.resize((SCREEN_W, SCREEN_H), Image.LANCZOS))
        return out

    def mouth_center(self, key: str, index: int):
        """Where this cell's mouth lands on screen once the eyes are pinned."""
        self._load(key)
        m = self._marks[key][index]
        s = self._scale[key]
        return (self.recipe.eye_center[0] + (m.mouth[0] - m.eye_center[0]) * s,
                self.recipe.eye_center[1] + (m.mouth[1] - m.eye_center[1]) * s)


def ss_rect(rect):
    return tuple(v * SS for v in rect)


def build(args) -> int:
    recipe = Recipe.load(args.character)
    lib = CellLibrary(args.source, recipe)
    pack_name = args.name or recipe.name
    pack_dir = args.out / "companion" / "packs" / pack_name
    started = time.time()
    total_bytes = 0
    files = 0
    preview = {}

    names = args.only if args.only else list(recipe.expressions)
    for name in names:
        if name not in recipe.expressions:
            raise SystemExit(f"{recipe.name}: no such expression {name!r}")

    print(f"character '{recipe.name}' ({recipe.theme} theme) -> pack '{pack_name}'")

    for name in names:
        t0 = time.time()
        spec = recipe.expressions[name]
        accent = spec["accent"]
        sheet_key, cell = spec["cell"]
        base_ss = lib.frame(sheet_key, cell, accent)
        base = base_ss.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)

        eye_tiles = []
        for _, skey, sidx in recipe.eye_slots:
            variant = lib.frame(skey, sidx, accent)
            grafted = ch.graft(base_ss, variant, ss_rect(recipe.eye_rect),
                               radius=14, blur=6.0, ring=20)
            eye_tiles.append(ch.crop_tile(grafted, recipe.eye_rect))

        mouth_tiles = []
        for _, skey, sidx in recipe.visemes:
            variant = lib.frame(skey, sidx, accent)
            grafted = ch.graft(base_ss, variant, ss_rect(recipe.mouth_rect),
                               radius=12, blur=6.0, ring=18)
            mouth_tiles.append(ch.crop_tile(grafted, recipe.mouth_rect))

        closed = eye_tiles[m5a.EYE_SLOTS.index("closed")]
        widest = mouth_tiles[min(4, len(mouth_tiles) - 1)]
        preview[name] = {"base": base, "closed": closed, "wide_mouth": widest}

        if not args.preview:
            for sub, frames in (("base", [base]), ("eyes", eye_tiles), ("mouth", mouth_tiles)):
                total_bytes += m5a.write_clip(pack_dir / sub / f"{name}.m5a", frames)
                files += 1
        print(f"  {name:<10} {time.time() - t0:5.1f}s")

    clips = {}
    if not args.preview and not args.no_gestures:
        neutral_accent = recipe.expressions[names[0]]["accent"]
        for gname, spec in recipe.gestures.items():
            t0 = time.time()
            frames = lib.clip_frames(spec["sheet"], spec["frames"], neutral_accent)
            total_bytes += m5a.write_clip(pack_dir / "clips" / f"{gname}.m5a", frames,
                                          fps=spec.get("fps", 10))
            files += 1
            clips[gname] = f"clips/{gname}.m5a"
            print(f"  clip {gname:<12} {len(frames)} frames  {time.time() - t0:5.1f}s")

    if args.preview:
        write_preview(preview, args.preview_path, recipe)
        return 0

    manifest = {
        "pack": pack_name,
        "character": recipe.name,
        "version": 2,
        "format": "rgb565be",
        "theme": THEMES[recipe.theme]["overlay"],
        "screen": {"w": SCREEN_W, "h": SCREEN_H},
        "eye_center": {"x": recipe.eye_center[0], "y": recipe.eye_center[1]},
        "sway_rect": {"x": 0, "y": 0, "w": 0, "h": 0},
        "eye_rect": dict(zip("xywh", recipe.eye_rect)),
        "mouth_rect": dict(zip("xywh", recipe.mouth_rect)),
        "sway_frames": 1,
        "eye_slots": [n for n, _, _ in recipe.eye_slots],
        "viseme_frames": len(recipe.visemes),
        "visemes": [n for n, _, _ in recipe.visemes],
        "expressions": {
            name: {"base": f"base/{name}.m5a", "sway": "",
                   "eyes": f"eyes/{name}.m5a", "mouth": f"mouth/{name}.m5a"}
            for name in names
        },
        "clips": clips,
    }
    pack_dir.mkdir(parents=True, exist_ok=True)
    (pack_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"\n{files} clips, {total_bytes / (1024 * 1024):.1f} MB "
          f"in {time.time() - started:.1f}s")
    print(f"pack -> {pack_dir}")
    return 0


def write_preview(preview, path: Path, recipe: Recipe):
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = 5
    rows = (len(preview) + cols - 1) // cols
    img = Image.new("RGB", (cols * (SCREEN_W + 8), rows * (SCREEN_H + 70)), (24, 26, 30))
    d = ImageDraw.Draw(img)
    for i, (name, parts) in enumerate(preview.items()):
        px, py = (i % cols) * (SCREEN_W + 8), (i // cols) * (SCREEN_H + 70)
        img.paste(parts["base"], (px, py))
        er, mr = recipe.eye_rect, recipe.mouth_rect
        d.rectangle((px + er[0], py + er[1], px + er[0] + er[2], py + er[1] + er[3]),
                    outline=(255, 140, 0))
        d.rectangle((px + mr[0], py + mr[1], px + mr[0] + mr[2], py + mr[1] + mr[3]),
                    outline=(255, 0, 200))
        img.paste(parts["closed"], (px, py + SCREEN_H + 4))
        img.paste(parts["wide_mouth"], (px + er[2] + 6, py + SCREEN_H + 4))
        d.text((px + 4, py + SCREEN_H + 58), name, fill=(230, 230, 230))
    img.save(path)
    print(f"preview -> {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parents[1]
    ap.add_argument("--source", type=Path, default=root / "assets/source")
    ap.add_argument("--out", type=Path, default=root / "build/sd",
                    help="SD card root; the pack lands in <out>/companion/packs/<name>")
    ap.add_argument("--character", type=Path,
                    default=root / "assets/characters/claudecode.json",
                    help="character recipe; one file per character")
    ap.add_argument("--name", default=None,
                    help="pack directory name on the card (default: the recipe's name)")
    ap.add_argument("--only", nargs="*", help="build a subset of expressions")
    ap.add_argument("--no-gestures", action="store_true")
    ap.add_argument("--preview", action="store_true", help="contact sheet only, no .m5a files")
    ap.add_argument("--preview-path", type=Path, default=root / "build/pack_preview.png")
    args = ap.parse_args()
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
