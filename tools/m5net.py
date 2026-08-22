#!/usr/bin/env python3
"""Talk to an M5Companion over the network instead of the cable.

The device serves a page for a person and this for a script. The two read the
same endpoints, so nothing here can tell you something the browser cannot -
but a terminal that follows the log while the device is on a shelf across the
room is the whole reason the endpoints exist.

    m5net.py status                 one snapshot of everything measurable
    m5net.py log -f                 follow the firmware log, like tail -f
    m5net.py boots                  why the last eight boots happened
    m5net.py push [firmware.bin]    upload a build and restart into it
    m5net.py serve [firmware.bin]   serve the build here and have the device
                                    fetch it - no inbound connection needed
    m5net.py reboot                 restart, recorded as deliberate
    m5net.py backups                what is on the card to fall back to
    m5net.py backup                 copy the running firmware to the card now
    m5net.py restore [file]         put a backup back and restart into it
    m5net.py recover                hand over to the recovery application
    m5net.py normal                 leave safe mode: clear the boot-loop count
    m5net.py power [seconds]        turn the loads on one at a time and see
                                    which one the battery cannot take

The device address comes from --host, then $M5_HOST, then the device.json on
the SD card image in build/, then CoreS3-Companion.local. If the device has an
`ota_password` set, pass --password or set $M5_PASSWORD; reading never needs
one, only the commands that change something.
"""

import argparse
import base64
import json
import mimetypes
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from functools import partial
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CARD_CONFIG = ROOT / "build" / "sd" / "companion" / "config" / "device.json"
# Both the cable env and the OTA env produce the same image in differently
# named directories, and PlatformIO deletes the one you are not currently
# building. So look for whatever is actually there, newest first, rather than
# naming one and being wrong half the time.
BIN_GLOB = ".pio/build/*/firmware.bin"


def default_bin() -> Path | None:
    builds = sorted(ROOT.glob(BIN_GLOB), key=lambda p: p.stat().st_mtime, reverse=True)
    return builds[0] if builds else None


def default_host() -> str:
    if os.environ.get("M5_HOST"):
        return os.environ["M5_HOST"]
    # The card image is where the device's own settings were last written
    # down, which makes it the best guess available without asking the network.
    # mDNS advertises the device_name, not a fixed name - the firmware passes
    # it straight to ArduinoOTA.setHostname - so a guess that ignores it is
    # wrong for every device whose name was ever changed.
    try:
        cfg = json.loads(CARD_CONFIG.read_text())
        if cfg.get("device_ip"):
            return cfg["device_ip"]
        if cfg.get("device_name"):
            return f"{cfg['device_name']}.local"
    except Exception:
        pass
    return "CoreS3-Companion.local"


# The device leaves reading open and guards anything that changes it. The
# username is fixed firmware-side; only the password is a setting.
PASSWORD = os.environ.get("M5_PASSWORD", "")


def auth_header() -> dict:
    if not PASSWORD:
        return {}
    token = base64.b64encode(f"m5:{PASSWORD}".encode()).decode()
    return {"Authorization": "Basic " + token}


def get(host: str, path: str, timeout: float = 5.0):
    req = urllib.request.Request(f"http://{host}{path}", headers=auth_header())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def post(host: str, path: str, timeout: float = 10.0, data: bytes = b"", headers=None):
    url = f"http://{host}{path}"
    head = dict(headers or {})
    head.update(auth_header())
    req = urllib.request.Request(url, data=data, headers=head, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def human_ms(ms: int) -> str:
    s = ms // 1000
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m {s % 60}s"
    return f"{s // 3600}h {(s % 3600) // 60}m"


def cmd_status(args) -> int:
    j = get(args.host, "/api/status")
    width = max(len(k) for k in j)
    for k, v in j.items():
        if k == "uptime_ms":
            v = f"{v} ({human_ms(v)})"
        print(f"{k:>{width}} : {v}")
    return 0


def cmd_boots(args) -> int:
    j = get(args.host, "/api/boot")
    print(f"{'#':>4} {'started as':<10} {'ran for':>10} {'batt':>6} {'low heap':>9}  ended")
    for b in j["history"]:
        ended = "on purpose" if b["clean"] else "WITHOUT WARNING"
        batt = f"{b['battery']}%" + ("+" if b["charging"] else "")
        print(f"{b['boot']:>4} {b['reason']:<10} {human_ms(b['uptime_ms']):>10} "
              f"{batt:>6} {b['min_heap'] // 1024:>8}k  {ended}")
    return 0


def cmd_log(args) -> int:
    seq = 0
    first = True
    while True:
        try:
            j = get(args.host, f"/api/log?since={seq}")
        except (urllib.error.URLError, socket.timeout, TimeoutError) as e:
            if not args.follow:
                raise
            print(f"[m5net] device unreachable ({e}); retrying", file=sys.stderr)
            time.sleep(2)
            continue
        if first:
            tail = j.get("previous", "")
            if tail:
                print("--- from before the last restart -------------------------")
                print(tail.rstrip())
                print("--- end of what survived ---------------------------------")
            first = False
        elif j["from"] > seq:
            print(f"--- {j['from'] - seq} bytes scrolled out ---")
        sys.stdout.write(j["text"])
        sys.stdout.flush()
        seq = j["upto"]
        if not args.follow:
            return 0
        time.sleep(args.interval)


def multipart(field: str, path: Path) -> tuple[bytes, str]:
    boundary = "----m5net" + os.urandom(8).hex()
    ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{path.name}"\r\n'
        f"Content-Type: {ctype}\r\n\r\n"
    ).encode() + path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


def cmd_push(args) -> int:
    path = Path(args.firmware) if args.firmware else default_bin()
    if not path or not path.is_file():
        print(f"no firmware to send: {path or ROOT / BIN_GLOB}", file=sys.stderr)
        return 2
    body, ctype = multipart("firmware", path)
    print(f"[m5net] uploading {path} ({len(body)} bytes) to {args.host}")
    try:
        status, text = post(args.host, "/api/ota", timeout=180,
                            data=body, headers={"Content-Type": ctype})
    except urllib.error.HTTPError as e:
        print(f"[m5net] refused: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        return 1
    print(f"[m5net] {status} {text}")
    return 0 if status == 200 else 1


def lan_address_towards(host: str) -> str:
    """The address of this machine as the device would see it.

    Asking the routing table beats asking the hostname: a laptop with a VPN up
    resolves its own name to something the device cannot reach.
    """
    target = host.split(":")[0]
    try:
        addr = socket.gethostbyname(target)
    except OSError:
        addr = "8.8.8.8"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((addr, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def cmd_serve(args) -> int:
    path = Path(args.firmware) if args.firmware else default_bin()
    if not path or not path.is_file():
        print(f"no firmware to serve: {path or ROOT / BIN_GLOB}", file=sys.stderr)
        return 2

    handler = partial(SimpleHTTPRequestHandler, directory=str(path.parent))
    httpd = HTTPServer(("0.0.0.0", args.port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = f"http://{lan_address_towards(args.host)}:{args.port}/{path.name}"
    print(f"[m5net] serving {path.parent} as {url}")
    if args.no_trigger:
        print("[m5net] not telling the device; ctrl-c to stop serving")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0

    print(f"[m5net] asking {args.host} to fetch it")
    query = urllib.parse.urlencode({"url": url})
    try:
        status, text = post(args.host, f"/api/ota/pull?{query}", timeout=300)
        # Reaching here at all means the device did not restart, so it failed.
        print(f"[m5net] {status} {text}", file=sys.stderr)
        return 1
    except urllib.error.HTTPError as e:
        print(f"[m5net] failed: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError):
        # The expected ending: the device installed the image and restarted
        # into it, taking the socket with it.
        print("[m5net] the device stopped answering, which is what success "
              "looks like here - it restarted into the new image")
    finally:
        httpd.shutdown()

    print("[m5net] waiting for it to come back")
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            j = get(args.host, "/api/status", timeout=2)
            print(f"[m5net] back: boot #{j['boot']}, fw {j['fw']}, up {human_ms(j['uptime_ms'])}")
            return 0
        except Exception:
            time.sleep(2)
    print("[m5net] it did not come back within a minute", file=sys.stderr)
    return 1


def cmd_backups(args) -> int:
    j = get(args.host, "/api/firmware")
    st, rc = j["store"], j["recovery"]
    print(f"running   {st['running']}  ({st['running_size']} bytes) in slot {rc['running']}")
    print(f"other     {rc['other']}: " +
          ("holds an image" if rc["other_bootable"] else "empty"))
    print("recovery  " + ("installed" if rc["factory"] else "NOT INSTALLED "
                          "(pio run -e recovery -t upload)"))
    state = ("on probation" if rc["on_trial"]
             else "confirmed" if rc["confirmed"] else "not confirmed yet")
    print(f"image     {state}; {rc['boot_streak']} failed boots"
          + ("  SAFE MODE" if rc["safe_mode"] else ""))
    print()
    if not st["files"]:
        print("no backups on the card yet")
        return 0
    for f in st["files"]:
        mark = " <- known good" if f["known_good"] else ""
        print(f"  {f['file']:<40} {f['size'] // 1024:>6} kB{mark}")
    return 0


def cmd_backup(args) -> int:
    status, text = post(args.host, "/api/firmware/backup", timeout=120)
    print(f"[m5net] {status} {text}")
    return 0 if status == 200 else 1


def _restarting(what: str) -> int:
    print(f"[m5net] {what}")
    return 0


def cmd_restore(args) -> int:
    query = "?file=" + urllib.parse.quote(args.file) if args.file else ""
    try:
        status, text = post(args.host, "/api/firmware/restore" + query, timeout=180)
        if status == 200:
            return _restarting("installed - restarting into it")
        print(f"[m5net] {status} {text}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError):
        return _restarting("the device stopped answering - it is restarting")


def cmd_recover(args) -> int:
    try:
        status, text = post(args.host, "/api/recovery/enter", timeout=10)
        if status == 200:
            return _restarting("restarting into the recovery application; "
                               "watch the screen")
        print(f"[m5net] {status} {text}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError):
        return _restarting("restarting into the recovery application")


def cmd_normal(args) -> int:
    try:
        post(args.host, "/api/recovery/normal", timeout=10)
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError):
        pass
    return _restarting("boot-loop counter cleared - restarting into a full boot")


def cmd_power(args) -> int:
    """Runs the staged load test and follows the log while it does.

    The device has no battery voltage to read - the IP5306 reports five levels
    and nothing else - so the only way to find what a tired cell cannot supply
    is to apply the loads separately. Each stage announces itself before it
    starts, so if the device dies partway the last line names the stage.
    """
    seq = get(args.host, "/api/log?since=0")["upto"]
    print(f"[m5net] starting; if it dies, run `m5net.py log` after the next boot")
    try:
        post(args.host, f"/api/power/test?seconds={args.seconds}", timeout=10)
    except (urllib.error.URLError, socket.timeout, TimeoutError):
        print("[m5net] the device stopped answering as the test began", file=sys.stderr)
        return 1

    deadline = time.time() + args.seconds * 8 + 30
    while time.time() < deadline:
        try:
            j = get(args.host, f"/api/log?since={seq}", timeout=3)
        except (urllib.error.URLError, socket.timeout, TimeoutError):
            # Expected while a stage holds the loop; the console cannot answer
            # and that is not the same as the device being gone.
            time.sleep(1)
            continue
        for row in j["text"].splitlines():
            if "power test" in row:
                print(row)
        seq = j["upto"]
        if "all stages survived" in j["text"]:
            return 0
        time.sleep(1)
    print("[m5net] the test did not report finishing - check `log prev` after a "
          "power cycle", file=sys.stderr)
    return 1


def cmd_reboot(args) -> int:
    try:
        status, text = post(args.host, "/api/reboot", timeout=5)
        print(f"[m5net] {status} {text}")
    except (urllib.error.URLError, socket.timeout, TimeoutError):
        print("[m5net] the device stopped answering - it is restarting")
    return 0


def main() -> int:
    global PASSWORD
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=default_host(),
                    help="device address (default: %(default)s)")
    ap.add_argument("--password", default=PASSWORD,
                    help="ota_password, if the device has one set ($M5_PASSWORD)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status").set_defaults(fn=cmd_status)
    sub.add_parser("boots").set_defaults(fn=cmd_boots)
    sub.add_parser("reboot").set_defaults(fn=cmd_reboot)
    sub.add_parser("backups").set_defaults(fn=cmd_backups)
    sub.add_parser("backup").set_defaults(fn=cmd_backup)
    sub.add_parser("recover").set_defaults(fn=cmd_recover)
    sub.add_parser("normal").set_defaults(fn=cmd_normal)

    p = sub.add_parser("power")
    p.add_argument("seconds", nargs="?", type=int, default=6,
                   help="how long to hold each stage (default: %(default)s)")
    p.set_defaults(fn=cmd_power)

    p = sub.add_parser("restore")
    p.add_argument("file", nargs="?",
                   help="a backup name from `backups`; the known-good one by default")
    p.set_defaults(fn=cmd_restore)

    p = sub.add_parser("log")
    p.add_argument("-f", "--follow", action="store_true")
    p.add_argument("--interval", type=float, default=1.0)
    p.set_defaults(fn=cmd_log)

    p = sub.add_parser("push")
    p.add_argument("firmware", nargs="?")
    p.set_defaults(fn=cmd_push)

    p = sub.add_parser("serve")
    p.add_argument("firmware", nargs="?")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--no-trigger", action="store_true",
                   help="serve the file but let something else start the update")
    p.set_defaults(fn=cmd_serve)

    args = ap.parse_args()
    PASSWORD = args.password
    try:
        return args.fn(args)
    except KeyboardInterrupt:
        return 130
    except urllib.error.HTTPError as e:
        print(f"[m5net] {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, socket.timeout, TimeoutError) as e:
        print(f"[m5net] cannot reach {args.host}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
