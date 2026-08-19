#!/usr/bin/env python3
"""Serial monitor that resets the board first, so boot logs are never missed."""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--no-reset", action="store_true")
    a = ap.parse_args()

    s = serial.Serial(a.port, a.baud, timeout=0.5)
    if not a.no_reset:
        # DTR/RTS wiggle is the M5Stack auto-reset circuit, same as esptool's.
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.12)
        s.setRTS(False)
        time.sleep(0.05)
        s.reset_input_buffer()

    end = time.time() + a.seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
