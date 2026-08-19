#!/usr/bin/env python3
"""Bridges the M5GO's USB cable to the companion server's WebSocket.

The device's own transport is Wi-Fi, which is right for the product and wrong
for a bench: provisioning credentials is the slowest step in every debug loop,
and a board on a desk is already holding a cable that flashes it. The firmware
carries the same protocol over that cable ("link on" in the serial console);
this relays it to a normal server, which therefore needs no changes and cannot
tell the difference.

    python tools/usb_link.py --url ws://127.0.0.1:8766/m5companion

Speech runs at 32 kB/s in each direction, so the link switches to 921600 baud
before it starts - 115200 would not carry one direction, let alone two.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import struct
import sys
import time

import serial
import websockets

# Big enough for one utterance frame plus the header, small enough that a
# stalled read is noticed inside one animation tick.
READ_CHUNK = 4096
RATE = 16000


def tone(seconds: float, freq: float = 220.0) -> bytes:
    n = int(RATE * seconds)
    return struct.pack(f"<{n}h", *(int(9000 * math.sin(2 * math.pi * freq * i / RATE))
                                   for i in range(n)))


class Link:
    """Frames on the wire: "@tx <len>\\n<json>" and "@txb <len>\\n<pcm>".

    Anything not matching that is a log line from the firmware, and is printed
    rather than dropped - the boot log is half the reason to watch this port.
    """

    def __init__(self, ser: serial.Serial, verbose: bool):
        self.ser = ser
        self.verbose = verbose
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[str, bytes]]:
        self.buf.extend(data)
        out: list[tuple[str, bytes]] = []
        while True:
            nl = self.buf.find(b"\n")
            if nl < 0:
                break
            line = bytes(self.buf[:nl])
            if line.startswith(b"@tx ") or line.startswith(b"@txb "):
                kind, _, size = line.partition(b" ")
                try:
                    want = int(size)
                except ValueError:
                    del self.buf[:nl + 1]
                    continue
                if len(self.buf) < nl + 1 + want:
                    break  # payload still arriving
                payload = bytes(self.buf[nl + 1:nl + 1 + want])
                del self.buf[:nl + 1 + want]
                out.append(("bin" if kind == b"@txb" else "text", payload))
            else:
                del self.buf[:nl + 1]
                text = line.decode("utf-8", "replace").rstrip()
                if text:
                    print(f"  device| {text}", flush=True)
        return out

    def send_text(self, payload: bytes):
        self.ser.write(b"rx %d\n" % len(payload))
        self.ser.write(payload)

    def send_binary(self, payload: bytes):
        self.ser.write(b"rxb %d\n" % len(payload))
        self.ser.write(payload)


def open_device(port: str, baud: int, transfer_baud: int, reset: bool) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=0.05, write_timeout=5.0)
    if reset:
        # The M5Stack auto-reset circuit, same wiggle esptool uses. Starting
        # from a known boot beats guessing what state the last run left.
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(2.5)
    ser.reset_input_buffer()

    ser.write(b"ping\n")
    time.sleep(0.4)
    reply = ser.read(4096).decode("utf-8", "replace")
    if "pong" not in reply:
        raise SystemExit(f"no response from {port}; is the firmware running?\n{reply}")
    print(f"device: {[l for l in reply.splitlines() if 'pong' in l][0].strip()}")

    if transfer_baud != baud:
        ser.write(b"baud %d\n" % transfer_baud)
        time.sleep(0.3)
        ser.read(256)
        ser.baudrate = transfer_baud
        time.sleep(0.2)
        ser.reset_input_buffer()
        print(f"link speed: {transfer_baud} baud")
    return ser


async def kick(ws, seconds: float):
    """Sends an utterance to the server as if the button had been held.

    Speech recognition is the last part of the pipeline to arrive, so during
    bring-up the audio content does not matter - what matters is that a real
    device shows the thinking face, then speaks the reply. This makes that
    testable without a hand on the board.
    """
    await asyncio.sleep(1.5)  # let the device's hello land first
    print(f"kick: sending a {seconds:.1f} s utterance", flush=True)
    await ws.send(json.dumps({"type": "listen.begin", "format": "pcm_s16le",
                              "rate": RATE}))
    pcm = tone(seconds)
    for off in range(0, len(pcm), 640):
        await ws.send(pcm[off:off + 640])
        await asyncio.sleep(0.016)
    await ws.send(json.dumps({"type": "listen.end"}))


async def run(url: str, ser: serial.Serial, verbose: bool, kick_seconds: float) -> int:
    link = Link(ser, verbose)
    loop = asyncio.get_running_loop()

    async with websockets.connect(url, max_size=2 ** 20) as ws:
        print(f"bridging {ser.port} <-> {url}", flush=True)
        ser.write(b"link on\n")

        async def serial_to_ws():
            while True:
                data = await loop.run_in_executor(
                    None, ser.read, max(1, min(READ_CHUNK, ser.in_waiting or 1)))
                for kind, payload in link.feed(data):
                    if kind == "text":
                        if verbose:
                            print(f"  device> {payload.decode('utf-8', 'replace')}", flush=True)
                        await ws.send(payload.decode("utf-8", "replace"))
                    else:
                        await ws.send(payload)

        async def ws_to_serial():
            async for message in ws:
                if isinstance(message, bytes):
                    link.send_binary(message)
                else:
                    if verbose:
                        print(f"  server> {message}", flush=True)
                    link.send_text(message.encode("utf-8"))

        tasks = [asyncio.create_task(serial_to_ws()), asyncio.create_task(ws_to_serial())]
        if kick_seconds:
            tasks.append(asyncio.create_task(kick(ws, kick_seconds)))
        done, pending = await asyncio.wait(
            tasks,
            return_when=asyncio.FIRST_EXCEPTION,
        )
        for task in pending:
            task.cancel()
        for task in done:
            exc = task.exception()
            if exc and not isinstance(exc, websockets.ConnectionClosed):
                raise exc
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--transfer-baud", type=int, default=921600)
    ap.add_argument("--url", default="ws://127.0.0.1:8765/m5companion")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--kick", type=float, default=0.0, metavar="SECONDS",
                    help="send a synthetic utterance once connected, so the "
                         "device can be exercised without holding the button")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    ser = open_device(a.port, a.baud, a.transfer_baud, not a.no_reset)
    try:
        return asyncio.run(run(a.url, ser, a.verbose, a.kick))
    except KeyboardInterrupt:
        return 0
    finally:
        try:
            ser.write(b"link off\n")
            ser.flush()
            # Put the port back where the next tool expects to find it. A run
            # that ends at 921600 looks, to anything opening at 115200, exactly
            # like a board with no firmware on it.
            if ser.baudrate != a.baud:
                ser.write(b"baud %d\n" % a.baud)
                ser.flush()
                time.sleep(0.2)
        except serial.SerialException:
            pass
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
