"""Writer for the .m5a container consumed by the firmware.

Mirrors include/AssetFormat.hpp. Frames are raw RGB565 in the panel's own byte
order, which is why playback on the device costs nothing but a memcpy.
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Sequence

import numpy as np

MAGIC = 0x3141354D  # 'M','5','A','1'
VERSION = 1
HEADER_BYTES = 32

FMT_RGB565_BE = 0
FMT_RGB565_LE = 1

# Must match m5a::EyeSlot in include/AssetFormat.hpp, in order.
EYE_SLOTS = (
    "open_center", "open_left", "open_right", "open_up", "open_down",
    "soft_lower", "half", "almost_closed", "closed", "wide",
    "sleepy_half", "sleepy_closed",
)


def rgb565_bytes(image, big_endian: bool = True) -> bytes:
    """Converts a PIL RGB image to packed RGB565."""
    if image.mode != "RGB":
        image = image.convert("RGB")
    a = np.asarray(image, dtype=np.uint16)
    v = ((a[:, :, 0] & 0xF8) << 8) | ((a[:, :, 1] & 0xFC) << 3) | (a[:, :, 2] >> 3)
    # ">u2" is the ILI9341 wire order; "<u2" is what the ESP32 stores natively.
    return v.astype(">u2" if big_endian else "<u2").tobytes()


def write_clip(path: Path, frames: Sequence, fps: int = 0, fmt: int = FMT_RGB565_BE,
               flags: int = 0) -> int:
    """Writes frames (PIL images, all the same size) to a .m5a file."""
    if not frames:
        raise ValueError(f"{path}: no frames")
    w, h = frames[0].size
    for f in frames:
        if f.size != (w, h):
            raise ValueError(f"{path}: frame size mismatch {f.size} != {(w, h)}")

    frame_bytes = w * h * 2
    header = struct.pack(
        "<IHHHHHHIB3sII",
        MAGIC, VERSION, flags, w, h, len(frames), fps, frame_bytes, fmt, b"\0\0\0", 0, 0,
    )
    assert len(header) == HEADER_BYTES, len(header)

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as fh:
        fh.write(header)
        for f in frames:
            fh.write(rgb565_bytes(f, fmt == FMT_RGB565_BE))
    return HEADER_BYTES + frame_bytes * len(frames)
