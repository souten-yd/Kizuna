#!/usr/bin/env python3
"""Pushes a pack onto the M5GO's SD card over the USB serial cable.

The M5GO cannot appear as a mass-storage device - its ESP32-D0WD has no USB
peripheral, and the socket is a CP210x UART bridge - so artwork normally means
moving the card between the board and a reader. This talks to the firmware's
serial console instead, and only sends files whose CRC differs, which makes a
re-pack of one expression a few seconds rather than a full 11 MB transfer.

    python tools/push_sd.py                     # sync build/sd
    python tools/push_sd.py --only base mouth   # sync some subdirectories
    python tools/push_sd.py --info              # just report device state
"""

from __future__ import annotations

import argparse
import binascii
import re
import sys
import time
from pathlib import Path

import serial

BLOCK = 4096

# ESP-IDF and Arduino-ESP32 log lines: "[12345][E][vfs_api.cpp:105] open(): ..."
# and the early-boot "E (282) psram: ..." form.
LOG_LINE = re.compile(r"^(\[\s*\d+\]\[[VDIWE]\]\[|[VDIWE] \(\d+\) )")


class Device:
    def __init__(self, port: str, baud: int, transfer_baud: int, verbose: bool):
        self.verbose = verbose
        self.ser = serial.Serial(port, baud, timeout=10)
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self._transfer_baud = transfer_baud

    # ------------------------------------------------------------- plumbing --
    def line(self, timeout: float = 10.0) -> str:
        # The firmware logs to this same port, so a reply is whatever is not a
        # log line. Reading one without checking desynchronises the whole
        # dialogue: the log lands as the answer to the previous command and
        # every reply after it is off by one.
        deadline = time.monotonic() + timeout
        while True:
            self.ser.timeout = max(0.1, deadline - time.monotonic())
            raw = self.ser.readline()
            if not raw:
                raise TimeoutError("no response from the device")
            text = raw.decode("utf-8", "replace").strip()
            if LOG_LINE.match(text):
                if self.verbose:
                    print(f"    . {text}")
                continue
            if self.verbose:
                print(f"    < {text}")
            return text

    def command(self, text: str) -> str:
        if self.verbose:
            print(f"    > {text}")
        self.ser.write((text + "\n").encode())
        self.ser.flush()
        return self.line()

    def _ping(self, tries: int) -> str | None:
        # The firmware may be mid-boot; give it a few tries before failing.
        for _ in range(tries):
            self.ser.reset_input_buffer()
            self.ser.write(b"ping\n")
            self.ser.flush()
            self.ser.timeout = 0.5
            for _ in range(6):
                raw = self.ser.readline()
                if raw.startswith(b"pong"):
                    return raw.decode().strip()
        return None

    def connect(self) -> str:
        reply = self._ping(20)
        if reply:
            return reply

        # A run that died mid-transfer left the device at the transfer speed,
        # and the next run then talks to it in a baud rate it cannot hear.
        # That looks exactly like a board with no firmware, so rather than say
        # so, look for it where it actually is.
        if self._transfer_baud != self.ser.baudrate:
            slow = self.ser.baudrate
            self.ser.baudrate = self._transfer_baud
            reply = self._ping(4)
            if reply:
                print(f"  device was left at {self._transfer_baud} baud; recovered")
                return reply
            self.ser.baudrate = slow

        raise SystemExit("no 'pong' from the device - is the firmware running?")

    def speed_up(self):
        if self._transfer_baud == self.ser.baudrate:
            return
        reply = self.command(f"baud {self._transfer_baud}")
        if reply != "ok":
            print(f"  staying at {self.ser.baudrate} baud ({reply})")
            return
        time.sleep(0.1)
        self.ser.baudrate = self._transfer_baud
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        if self.command("ping").startswith("pong"):
            print(f"  transfer speed: {self._transfer_baud} baud")
        else:
            raise SystemExit("device stopped responding after the baud change")

    def slow_down(self, baud: int):
        if self.ser.baudrate == baud:
            return
        try:
            self.command(f"baud {baud}")
        except TimeoutError:
            pass
        self.ser.baudrate = baud

    # -------------------------------------------------------------- actions --
    def stat(self, path: str):
        reply = self.command(f"stat {path}")
        if reply.startswith("size "):
            parts = reply.split()
            return int(parts[1]), int(parts[3])
        return None

    def put(self, path: str, data: bytes) -> bool:
        crc = binascii.crc32(data) & 0xFFFFFFFF
        reply = self.command(f"put {path} {len(data)} {crc}")
        if reply != "ready":
            print(f"    device refused: {reply}")
            return False

        sent = 0
        while sent < len(data):
            chunk = data[sent:sent + BLOCK]
            self.ser.write(chunk)
            self.ser.flush()
            sent += len(chunk)
            ack = self.line(timeout=15.0)
            if not ack.startswith("ack"):
                print(f"    transfer failed: {ack}")
                return False
        final = self.line(timeout=20.0)
        if not final.startswith("ok"):
            print(f"    transfer failed: {final}")
            return False
        return True

    def info(self) -> str:
        return self.command("info")

    def reload(self) -> str:
        return self.command("reload")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--transfer-baud", type=int, default=921600)
    ap.add_argument("--source", type=Path, default=root / "build/sd")
    ap.add_argument("--only", nargs="*", help="limit to these path fragments")
    ap.add_argument("--force", action="store_true", help="send even when the CRC matches")
    ap.add_argument("--info", action="store_true", help="print device state and exit")
    ap.add_argument("--no-reload", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    dev = Device(args.port, args.baud, args.transfer_baud, args.verbose)
    print(dev.connect())

    if args.info:
        print(dev.info())
        return 0

    files = sorted(p for p in args.source.rglob("*") if p.is_file())
    if args.only:
        files = [p for p in files
                 if any(frag in str(p.relative_to(args.source)) for frag in args.only)]
    if not files:
        raise SystemExit(f"nothing to send from {args.source}")

    dev.speed_up()

    sent_bytes = 0
    skipped = 0
    started = time.time()
    try:
        for i, path in enumerate(files, 1):
            remote = "/" + str(path.relative_to(args.source)).replace("\\", "/")
            data = path.read_bytes()
            crc = binascii.crc32(data) & 0xFFFFFFFF

            if not args.force:
                current = dev.stat(remote)
                if current == (len(data), crc):
                    skipped += 1
                    continue

            print(f"[{i}/{len(files)}] {remote}  {len(data) / 1024:.0f} KB")
            t0 = time.time()
            if not dev.put(remote, data):
                return 1
            sent_bytes += len(data)
            dt = max(1e-3, time.time() - t0)
            print(f"    {len(data) / dt / 1024:.0f} KB/s")

        elapsed = time.time() - started
        print(f"\nsent {sent_bytes / 1024 / 1024:.2f} MB, skipped {skipped} unchanged "
              f"file(s), {elapsed:.1f}s")
        if sent_bytes and not args.no_reload:
            print("reload:", dev.reload())
    finally:
        dev.slow_down(args.baud)
    return 0


if __name__ == "__main__":
    sys.exit(main())
