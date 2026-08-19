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


TILE = 16
TILE_BYTES = TILE * TILE * 2
FLAG_TILE_DELTA = 1 << 1


def write_delta_clip(path: Path, frames: Sequence, fps: int = 0,
                     fmt: int = FMT_RGB565_BE, threshold: int = 12) -> int:
    """Writes frames as the 16x16 tiles that changed since the frame before.

    A full 320x240 frame is 150 KB, and the device's LCD and SD card share one
    SPI bus carrying about 850 KB/s together, so whole frames play at 5.5 fps
    no matter how short the clip is. Most of a gesture frame is identical to
    the one before it; sending only what moved is what makes head motion read
    as motion rather than as a slideshow.

    `threshold` is per channel, on the 8-bit values before packing. Zero would
    dirty half the screen on dithering noise alone.
    """
    if not frames:
        raise ValueError(f"{path}: no frames")
    w, h = frames[0].size
    for f in frames:
        if f.size != (w, h):
            raise ValueError(f"{path}: frame size mismatch {f.size} != {(w, h)}")
    if w % TILE or h % TILE:
        raise ValueError(f"{path}: {w}x{h} is not a whole number of {TILE} px tiles")

    tiles_x, tiles_y = w // TILE, h // TILE
    rgb = [np.asarray(f.convert("RGB"), dtype=np.int16) for f in frames]

    payloads = []
    for i, frame in enumerate(frames):
        if i == 0:
            keep = np.ones((tiles_y, tiles_x), dtype=bool)   # frame 0 is a keyframe
        else:
            diff = np.abs(rgb[i] - rgb[i - 1]).max(axis=2) > threshold
            keep = diff.reshape(tiles_y, TILE, tiles_x, TILE).any(axis=(1, 3))
        idx = np.flatnonzero(keep.reshape(-1)).astype("<u2")
        packed = rgb565_bytes(frame, fmt == FMT_RGB565_BE)
        rows = np.frombuffer(packed, dtype=np.uint8).reshape(h, w * 2)
        chunks = []
        for t in idx:
            ty, tx = divmod(int(t), tiles_x)
            block = rows[ty * TILE:(ty + 1) * TILE, tx * TILE * 2:(tx + 1) * TILE * 2]
            chunks.append(block.tobytes())
        payloads.append(struct.pack("<H", len(idx)) + idx.tobytes() + b"".join(chunks))

    table_bytes = (len(frames) + 1) * 4
    start = HEADER_BYTES + table_bytes
    offsets = [start]
    for p in payloads:
        offsets.append(offsets[-1] + len(p))

    header = struct.pack(
        "<IHHHHHHIB3sII",
        MAGIC, VERSION, FLAG_TILE_DELTA, w, h, len(frames), fps,
        max(len(p) for p in payloads), fmt, b"\0\0\0", 0, 0,
    )
    assert len(header) == HEADER_BYTES, len(header)

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as fh:
        fh.write(header)
        fh.write(struct.pack(f"<{len(offsets)}I", *offsets))
        for p in payloads:
            fh.write(p)
    return offsets[-1]
