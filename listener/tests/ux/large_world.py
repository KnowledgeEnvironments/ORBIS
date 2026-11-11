#!/usr/bin/env python3
"""A client on a world with a long history: usable in seconds, whatever the log holds.

    privateworld.py --port 7962 -- large_world.py --listener <binary> --url {url} --token-file {token} --root {root} --events 20000 --shots <dir>

Seeds Objects and Fields through the protocol, then times a new client from launch to attached, to
folds current, and to the History pane showing the head; it scrolls History back and sees older
rows arrive. Exit 0 on pass, 1 on failure.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools"), os.path.join(HERE, "..", "..", "..", "scripts", "bench")]
from drive import Drive  # noqa: E402
import ws as worldws  # noqa: E402


def seed(root_token, port, want):
    """Objects and Fields up to head `want`, through the protocol, in batches: 1 in 8 is a new Object."""
    w = worldws.World(token_file=root_token, port=port, kind="seed")
    head = w.call("history", {"last": 1}).get("seq") or 0
    started = time.time()
    i = head
    while head < want:
        frames = []
        for _ in range(400):
            i += 1
            if i % 8 == 0:
                frames.append(("act", {"class": "object", "action": "create",
                                       "args": {"fields": {"seed/n": {"type": "number", "value": i}}}}))
            else:
                frames.append(("set-field", {"object": w.me, "field": f"seed/f{i % 97}", "type": "string",
                                             "value": f"value {i}", "reader": "both"}))
        for n, (op, args) in enumerate(frames):
            w.s.sendall(worldws._frame(json.dumps({"id": f"s{i}-{n}", "op": op, "args": args})))
        got = 0
        while got < len(frames):
            code, payload = w._read()
            if code == 9:
                w.s.sendall(bytes([0x8a, 0x80]) + os.urandom(4))
            elif code == 1:
                got += 1
        head = w.call("history", {"last": 1}).get("seq") or 0
    print(f"[large] head {head}, {time.time() - started:.0f} s", flush=True)
    return head


def rss_mb(pid):
    return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True).strip()) / 1024.0


def launch(binary, url, token, home):
    sock = os.path.join(home, "d.sock")
    if os.path.exists(sock):
        os.remove(sock)
    return subprocess.Popen([binary, "--url", url, "--format", "json", "--token-file", token,
                             "--data-dir", home, "--drive", sock],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), sock


def attach_once(binary, a):
    """A client attaches and quits, so it comes back holding a cursor far behind the head."""
    home = tempfile.mkdtemp(prefix="orbis-large")
    proc, sock = launch(binary, a.url, a.token_file, home)
    d = Drive(sock)
    end = time.time() + 60
    while time.time() < end and not d.state().get("attached"):
        time.sleep(0.2)
    d.wait("settle", frames=8, timeout_ms=15000)
    time.sleep(1.5)
    proc.terminate()
    proc.wait(10)
    return home


def measure(binary, home, a, head, label, page):
    t0 = time.time()
    proc, sock = launch(binary, a.url, a.token_file, home)
    result = {"attached": None, "folds": None, "history": None}
    try:
        d = Drive(sock)
        d.sock.settimeout(120)
        end = time.time() + a.give_up
        while time.time() < end:
            s = d.state()
            now = time.time() - t0
            if result["attached"] is None and s.get("attached"):
                result["attached"] = now
            fold = s.get("fold_applied") or {}
            if result["folds"] is None and min(fold.get("principals", 0), fold.get("evaluations", 0)) >= head:
                result["folds"] = now
            if result["history"] is None and (s.get("head_seq") or 0) >= head:
                result["history"] = now
            if None not in result.values():
                break
            time.sleep(0.25)
        s = d.state()
        print(f"[large] {label}: attached {result['attached']} folds {result['folds']} history {result['history']} s; "
              f"rss {rss_mb(proc.pid):.0f} MB; rows {s.get('history_rows')} oldest {s.get('oldest_seq')}; "
              f"state {len(json.dumps(s))} bytes; {(s.get('boot') or {}).get('line')}", flush=True)
        if page:
            d.screenshot(os.path.join(a.shots, "large.png"))
            if "button:history-expand" in d.state().get("targets", []):
                d.click("button:history-expand")
                d.wait("settle", frames=6, timeout_ms=10000)
            before = d.state()
            for _ in range(6):
                d.scroll("history-rows", 40000)
                d.wait("settle", frames=6, timeout_ms=10000)
                time.sleep(0.4)
            after = d.state()
            print(f"[large] scrolled back: oldest {before.get('oldest_seq')} -> {after.get('oldest_seq')}, "
                  f"rows {before.get('history_rows')} -> {after.get('history_rows')}", flush=True)
            d.screenshot(os.path.join(a.shots, "large-older.png"))
            result["paged"] = (after.get("oldest_seq") or 0) < (before.get("oldest_seq") or 0)
        return result
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--baseline", help="an older client to time on the same world")
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--events", type=int, default=20000)
    ap.add_argument("--shots", required=True)
    ap.add_argument("--limit-s", type=float, default=10.0)
    ap.add_argument("--give-up", type=float, default=120.0)
    a = ap.parse_args()
    os.makedirs(a.shots, exist_ok=True)
    port = int(re.search(r":(\d+)/", a.url).group(1))
    home = attach_once(a.listener, a)
    old = attach_once(a.baseline, a) if a.baseline else None
    head = seed(a.root, port, a.events)
    if old:
        measure(a.baseline, old, a, head, "baseline, returning", False)
    r = measure(a.listener, home, a, head, "returning", True)
    fresh = measure(a.listener, tempfile.mkdtemp(prefix="orbis-large"), a, head, "new client", False)
    failures = []
    for label, m in (("returning", r), ("new client", fresh)):
        for key in ("folds", "history"):
            if m[key] is None or m[key] > a.limit_s:
                failures.append(f"{label} {key} at {m[key]}")
    if not r.get("paged"):
        failures.append("scrolling back paged nothing in")
    print("[large] FAIL " + str(failures) if failures else "[large] ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
