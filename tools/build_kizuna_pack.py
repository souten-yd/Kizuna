#!/usr/bin/env python3
"""Build the bundled Kizuna companion pack with the normal asset pipeline.

A fresh clone already contains every source sheet referenced by
assets/characters/kizuna.json, so this wrapper is intentionally boring: it
invokes the same packer used by the hardware-tested Claude Code pack and only
selects the Kizuna recipe/name. Extra arguments are forwarded unchanged.
"""

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
