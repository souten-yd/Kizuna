#!/usr/bin/env python3
"""Fast structural/landmark validation for the bundled Kizuna source sheets."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_kizuna_pack as kb  # noqa: E402
import m5a  # noqa: E402


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    recipe_path = root / "assets" / "characters" / "kizuna.json"
    source_dir = root / "assets" / "source"
    recipe = json.loads(recipe_path.read_text())

    errors: list[str] = []
    cell_count: dict[str, int] = {}
    for key, filename in recipe["sheets"].items():
        path = source_dir / filename
        if not path.exists():
            errors.append(f"{key}: missing {path}")
            continue
        image = Image.open(path).convert("RGB")
        if image.width % 4 or image.height % 4:
            errors.append(f"{key}: dimensions {image.size} do not divide into a 4x4 grid")
            continue
        cw, ch = image.width // 4, image.height // 4
        cell_count[key] = 16
        for row in range(4):
            for col in range(4):
                idx = row * 4 + col
                cell = image.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
                try:
                    kb.find_anchor(cell)
                except ValueError as exc:
                    errors.append(f"{key}: cell {idx}: {exc}")

    eye_names = [entry[0] for entry in recipe["eye_slots"]]
    if eye_names != list(m5a.EYE_SLOTS):
        errors.append(f"eye slot order mismatch: {eye_names}")

    def check_ref(kind: str, sheet_key: str, cell: int) -> None:
        if sheet_key not in recipe["sheets"]:
            errors.append(f"{kind}: unknown sheet {sheet_key}")
            return
        if cell < 0 or cell >= cell_count.get(sheet_key, 16):
            errors.append(f"{kind}: cell {cell} out of range for {sheet_key}")

    for name, sheet_key, cell in recipe["eye_slots"]:
        check_ref(f"eye {name}", sheet_key, int(cell))
    for name, sheet_key, cell in recipe["visemes"]:
        check_ref(f"viseme {name}", sheet_key, int(cell))
    for name, spec in recipe["expressions"].items():
        check_ref(f"expression {name}", spec["cell"][0], int(spec["cell"][1]))
    for name, spec in recipe.get("gestures", {}).items():
        for cell in spec["frames"]:
            check_ref(f"gesture {name}", spec["sheet"], int(cell))
        if not 1 <= int(spec.get("fps", 0)) <= 5:
            errors.append(f"gesture {name}: fps must be 1..5 for full-screen playback")

    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    print(f"Kizuna sources OK: {len(recipe['expressions'])} expressions, "
          f"{len(recipe['eye_slots'])} eye slots, {len(recipe['visemes'])} visemes, "
          f"{len(recipe.get('gestures', {}))} gestures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
