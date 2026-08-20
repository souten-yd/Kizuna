#!/usr/bin/env python3
"""Talks to the companion server the way the device does, with real speech.

fake_m5go.py sends a tone, which exercises the transport and nothing else: a
tone transcribes to silence, so the recogniser, the model and the synthesiser
are never asked to do anything. This connects as a device, streams an utterance
that has words in it, and reports what comes back - which is the only way to
check the reply path without pressing the button and talking.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))


async def main() -> int:
    import httpx
    import websockets
    import backends as be

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="+", help="what to say; each is one turn")
    ap.add_argument("--url", default="ws://127.0.0.1:8766/m5companion")
    ap.add_argument("--nas", default="http://192.168.68.57:11435")
    ap.add_argument("--rate", type=int, default=12000,
                    help="the device captures at 12 kHz and says so in "
                         "listen.begin; the server resamples")
    ap.add_argument("--timeout", type=float, default=300.0)
    a = ap.parse_args()

    async with httpx.AsyncClient(timeout=180.0) as client:
        spoken = []
        for line in a.text:
            r = await client.post(f"{a.nas}/v1/audio/speech",
                                  params={"profile": "m5go"},
                                  json={"text": line, "lang": "ja",
                                        "backend": "piper_plus", "speed": 1.0,
                                        "sample_rate": be.SAMPLE_RATE})
            r.raise_for_status()
            pcm = be.wav_to_pcm16(r.content)
            spoken.append(be.resample_linear(pcm, be.SAMPLE_RATE, a.rate))

    async with websockets.connect(a.url, max_size=None) as ws:
        await ws.send(json.dumps({
            "type": "hello", "device": "say_to_server", "name": "harness",
            "protocol": 2, "fw": "0.0.0", "audio_format": "pcm_s16le",
            "audio_rate": be.SAMPLE_RATE, "chunk_samples": 320}))

        for turn, (line, pcm) in enumerate(zip(a.text, spoken), 1):
            print(f"\n--- turn {turn}: {line}")
            await ws.send(json.dumps({"type": "listen.begin", "rate": a.rate}))
            step = a.rate * 2 // 50            # 20 ms, as the device sends it
            for i in range(0, len(pcm), step):
                await ws.send(pcm[i:i + step])
                await asyncio.sleep(0.02)      # real time, so the server paces
            await ws.send(json.dumps({"type": "listen.end"}))

            started = time.monotonic()
            audio = 0
            first = None
            while time.monotonic() - started < a.timeout:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=a.timeout)
                except asyncio.TimeoutError:
                    print("    timed out waiting for the reply")
                    break
                if isinstance(msg, bytes):
                    if first is None:
                        first = time.monotonic() - started
                    audio += len(msg)
                    continue
                event = json.loads(msg)
                kind = event.get("type")
                if kind == "state":
                    print(f"    state -> {event.get('state')}")
                    if event.get("state") == "idle":
                        break
                elif kind == "expression":
                    print(f"    expression -> {event.get('name')}")
            print(f"    {audio / (be.SAMPLE_RATE * 2):.1f} s of speech back, "
                  f"first audio {first:.1f} s" if first else
                  f"    no audio came back")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
