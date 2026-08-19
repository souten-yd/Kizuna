#!/usr/bin/env python3
"""Fast structural validation for the bundled Kizuna source sheets/recipe."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m5a  # noqa: E402
import sheet as sh  # noqa: E402


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
        rows = [row for row in sh.find_cells(Image.open(path)) if len(row) == 4]
        count = sum(len(row) for row in rows)
        cell_count[key] = count
        if len(rows) != 4 or count != 16:
            errors.append(f"{key}: expected 4x4 cells, got rows={[len(r) for r in rows]}")

    eye_names = [entry[0] for entry in recipe["eye_slots"]]
    if eye_names != list(m5a.EYE_SLOTS):
        errors.append(f"eye slot order mismatch: {eye_names}")

    if len(recipe.get("visemes", [])) != 8:
        errors.append(f"expected 8 visemes, got {len(recipe.get('visemes', []))}")

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
        fps = int(spec.get("fps", 0))
        if not 1 <= fps <= 5:
            errors.append(f"gesture {name}: fps must be 1..5 for full-screen playback")

    if len(recipe.get("expressions", {})) != 33:
        errors.append(f"expected 33 expression intents, got {len(recipe.get('expressions', {}))}")

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
