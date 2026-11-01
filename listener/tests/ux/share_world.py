#!/usr/bin/env python3
"""A person's terminal shared with one agent, from the tab's menu: the dialog, the red indicator, OFF.

    privateworld.py --port 7825 -- share_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

Nothing here reaches the main world: privateworld refuses its port. Exit 0 on pass, 1 on failure.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
from drive import Drive  # noqa: E402


def until(what, check, seconds=30.0):
    end = time.time() + seconds
    while time.time() < end:
        if check():
            return
        time.sleep(0.08)
    raise AssertionError("timed out waiting for " + what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-s-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "share.log"), "w"))
    settle = lambda frames=8: d.wait("settle", frames=frames, timeout_ms=15000)
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("the fold", lambda: "principal:root" in d.targets())
        d.click("tab:panes")
        settle()
        d.pane_open(kind="terminal", target="grid.shell")
        until("a terminal pane", lambda: [p for p in d.panes()["live"] if p.get("kind") == "terminal"])
        settle()
        known = ("tab:chat", "tab:graph", "tab:panes", "tab:principals", "tab:history", "tab:inspector", "tab:person", "tab:root", "tab:grid", "tab:inspect",
                 "tab:transcript", "tab:conversations", "tab:instructions", "tab:fields", "tab:memory",
                 "tab:devices", "tab:rules", "tab:you", "tab:messages")
        tab = next(x for x in d.targets() if x.startswith("tab:") and not x.startswith(known))
        d.call("click", target=tab, button="right")
        settle()
        d.click("menu:share-pane")
        settle()
        until("the agents", lambda: "button:share-agent:root" in d.targets(), 10.0)
        d.click("button:share-agent:root")
        settle()
        d.click("button:share-mode:type")
        settle()
        d.screenshot(os.path.join(shots, "share-dialog.png"))
        d.click("button:share-30")
        until("the red indicator", lambda: "unsafe" in d.targets(), 15.0)
        settle()
        d.screenshot(os.path.join(shots, "shared.png"))
        d.click("unsafe")
        settle()
        off = next(x for x in d.targets() if x.startswith("button:unsafe-off:"))
        d.screenshot(os.path.join(shots, "shared-list.png"))
        d.click(off)
        until("the indicator gone", lambda: "unsafe" not in d.targets(), 15.0)
        print("[share] ok")
        return 0
    except Exception as e:  # noqa: BLE001
        print("[share] FAIL", repr(e)[:300])
        try:
            d.screenshot(os.path.join(shots, "fail.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
