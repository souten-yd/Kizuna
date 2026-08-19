#!/usr/bin/env python3
"""Pretends to be an M5GO so the server can be tested without hardware.

Speaks the same protocol the firmware speaks: hello, a push-to-talk utterance
of 20 ms PCM chunks, then whatever the server sends back. Reports what it
received so a broken pipeline is obvious from the exit output alone.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import struct
import sys

import websockets

RATE = 16000
CHUNK = 320  # samples, i.e. 20 ms


def tone(seconds: float, freq: float = 220.0) -> bytes:
    n = int(RATE * seconds)
    return struct.pack(f"<{n}h", *(int(9000 * math.sin(2 * math.pi * freq * i / RATE))
                                  for i in range(n)))


async def main(url: str, seconds: float, timeout: float) -> int:
    received_audio = 0
    events: list[str] = []

    async with websockets.connect(url, max_size=2 ** 20) as ws:
        await ws.send(json.dumps({
            "type": "hello", "device": "m5go", "name": "fake-m5go",
            "protocol": 2, "fw": "0.2.0",
            "audio_format": "pcm_s16le", "audio_rate": RATE, "chunk_samples": CHUNK,
        }))

        await ws.send(json.dumps({"type": "listen.begin", "format": "pcm_s16le",
                                  "rate": RATE}))
        pcm = tone(seconds)
        for off in range(0, len(pcm), CHUNK * 2):
            await ws.send(pcm[off:off + CHUNK * 2])
            await asyncio.sleep(0.002)
        await ws.send(json.dumps({"type": "listen.end"}))

        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                break
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=remaining)
            except asyncio.TimeoutError:
                break
            if isinstance(msg, bytes):
                received_audio += len(msg)
                continue
            obj = json.loads(msg)
            events.append(obj.get("type", "?"))
            print("<", msg)
            if obj.get("type") == "state" and obj.get("state") == "idle":
                # The server finished the turn; give straggling audio a moment.
                deadline = min(deadline, asyncio.get_running_loop().time() + 1.0)

    print(f"\nevents: {events}")
    print(f"audio received: {received_audio} bytes "
          f"({received_audio / (RATE * 2):.2f} s)")

    ok = "expression" in events or "state" in events
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="ws://127.0.0.1:8765/m5companion")
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--timeout", type=float, default=25.0)
    a = ap.parse_args()
    sys.exit(asyncio.run(main(a.url, a.seconds, a.timeout)))
