#!/usr/bin/env python3
"""Plays a WAV on the M5GO's speaker over the USB link, and nothing else.

Speech arriving gritty could be the synthesiser, the rate conversion, the
pacing, the queue, the amplifier or the DAC. Each of those is cheap to rule
out only if something can put a known signal on the speaker without the rest
of the pipeline in the way. That is this.

    python tools/play_wav.py tone           # 1 kHz sine, 3 s
    python tools/play_wav.py sweep          # 100 Hz to 7 kHz
    python tools/play_wav.py reply.wav      # any WAV, any rate
"""

from __future__ import annotations

import argparse
import array
import asyncio
import json
import math
import sys
import time
import wave
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))

RATE = 16000
CHUNK = 640
CHUNK_SECONDS = CHUNK / (RATE * 2)
PRIME = 8


def synth(kind: str, seconds: float = 1.5, level: float = 0.12) -> bytes:
    """A quiet test signal.

    Deliberately quiet and short by default: a continuous sine at three
    quarters of full scale is indistinguishable from an alarm, and this gets
    run on a desk where someone is sitting. `--level` raises it when the
    quiet version has been listened to and something louder is wanted.
    """
    n = int(RATE * seconds)
    out = array.array("h", bytes(n * 2))
    amplitude = 32767 * max(0.0, min(1.0, level))
    for i in range(n):
        t = i / RATE
        if kind == "tone":
            v = math.sin(2 * math.pi * 440 * t)
        elif kind == "sweep":
            f = 200 * (20 ** (t / seconds))
            v = math.sin(2 * math.pi * f * t * 0.5)
        else:
            raise SystemExit(f"unknown signal {kind!r}")
        # A long fade at both ends: an abrupt start is a click, and a click is
        # exactly the artefact this tool exists to tell apart from grit.
        ramp = min(1.0, t / 0.15, (seconds - t) / 0.15)
        out[i] = int(amplitude * ramp * ramp * v)
    return out.tobytes()


def load(path: Path) -> bytes:
    import backends as be

    with wave.open(str(path), "rb") as w:
        pcm = w.readframes(w.getnframes())
        if w.getnchannels() == 2:
            pcm = be.stereo_to_mono(pcm)
        return be.resample_linear(pcm, w.getframerate(), RATE)


async def send(url_serial, pcm: bytes, verbose: bool):
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "usb_link", Path(__file__).resolve().parent / "usb_link.py")
    ul = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ul)
    link = ul.Link(url_serial, verbose)

    link.send_text(json.dumps({"type": "speech.begin", "format": "pcm_s16le",
                               "rate": RATE}).encode())
    started = time.monotonic()
    for index, off in enumerate(range(0, len(pcm), CHUNK)):
        link.send_binary(pcm[off:off + CHUNK])
        if index < PRIME:
            continue
        ahead = started + (index - PRIME + 1) * CHUNK_SECONDS - time.monotonic()
        if ahead > 0:
            await asyncio.sleep(ahead)
    link.send_text(json.dumps({"type": "speech.end"}).encode())
    await asyncio.sleep(1.0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="tone, sweep, or a path to a WAV")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--transfer-baud", type=int, default=921600)
    ap.add_argument("--level", type=float, default=0.12,
                    help="peak amplitude, 0..1. The default is quiet on purpose")
    ap.add_argument("--seconds", type=float, default=1.5)
    ap.add_argument("--shape", action="store_true",
                    help="apply the speaker conditioning the server applies")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.source in ("tone", "sweep"):
        pcm = synth(a.source, a.seconds, a.level)
    else:
        pcm = load(Path(a.source))
        if a.level != 1.0:
            import numpy as np
            x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
            peak = float(np.abs(x).max()) or 1.0
            x = x * (a.level * 32767 / peak)
            pcm = np.clip(x, -32768, 32767).astype(np.int16).tobytes()
    if a.shape:
        import backends as be
        pcm = be.shape_for_speaker(pcm)
    print(f"{len(pcm)} bytes, {len(pcm) / (RATE * 2):.2f} s")

    ser = serial.Serial(a.port, a.baud, timeout=0.2, write_timeout=5)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.12)
    ser.setRTS(False)
    time.sleep(3.0)
    # Retry rather than read once: the boot log is thousands of bytes and a
    # single read after a single ping usually returns the tail of that instead
    # of the reply. Also look at the transfer rate, where a killed run leaves
    # the device.
    def ping(tries: int) -> bool:
        for _ in range(tries):
            ser.reset_input_buffer()
            ser.write(b"\nping\n")
            ser.flush()
            deadline = time.monotonic() + 1.5
            while time.monotonic() < deadline:
                if b"pong" in ser.readline():
                    return True
        return False

    if not ping(6):
        ser.baudrate = a.transfer_baud
        if ping(3):
            print(f"  device was left at {a.transfer_baud} baud; recovered")
            ser.write(b"baud %d\n" % a.baud)
            ser.flush()
            time.sleep(0.3)
            ser.baudrate = a.baud
        else:
            raise SystemExit("no pong; is the firmware running?")
    ser.write(b"baud %d\n" % a.transfer_baud)
    time.sleep(0.3)
    ser.read(256)
    ser.baudrate = a.transfer_baud
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b"link on\n")
    time.sleep(0.3)
    ser.read(4000)

    try:
        asyncio.run(send(ser, pcm, a.verbose))
    finally:
        ser.write(b"link off\n")
        ser.flush()
        time.sleep(0.2)
        ser.write(b"baud %d\n" % a.baud)
        ser.flush()
        time.sleep(0.2)
        ser.close()
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
