#!/usr/bin/env python3
"""Build the bundled Kizuna companion pack with the normal asset pipeline."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    cmd = [
        sys.executable,
        str(root / "tools" / "pack_assets.py"),
        "--character",
        str(root / "assets" / "characters" / "kizuna.json"),
        "--name",
        "kizuna",
    ]
    cmd.extend(sys.argv[1:])
    return subprocess.call(cmd, cwd=root)


if __name__ == "__main__":
    raise SystemExit(main())
