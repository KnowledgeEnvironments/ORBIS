#!/usr/bin/env python3
"""A Message row in HISTORY opens its chat at that Message, and a Message goes back to its row.

    privateworld.py --port 7941 -- python3 jump_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

Nothing here reaches the main world: privateworld refuses its port. Exit 0 on pass, 1 on failure.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
from drive import Drive  # noqa: E402


def until(what, check, seconds=40.0):
    end = time.time() + seconds
    while time.time() < end:
        got = check()
        if got:
            return got
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=15000)


def first_seq(d):
    return int(d.state().get("head", 0)) * 2 // 3


def targets(d):
    return d.state().get("targets") or {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", required=True)
    a = ap.parse_args()
    os.makedirs(a.shots, exist_ok=True)
    work = tempfile.mkdtemp(prefix="orbis-jump-")
    sock = os.path.join(work, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--format", "json", "--token-file", a.token_file,
                             "--data-dir", work, "--drive", sock],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        d = Drive(sock)
        until("attach", lambda: d.state().get("attached"))
        settle(d)
        d.click("principal:root")
        until("input", lambda: "input" in targets(d))
        settle(d)
        # several turns, so the first Message scrolls away
        for i in range(14):
            d.click("input")
            d.type(f"line {i} of the jump test")
            d.key("return")
            settle(d, 8)
            time.sleep(0.2)
        time.sleep(2.0)
        settle(d)
        d.click("button:history-expand") if "button:history-expand" in targets(d) else None
        settle(d, 10)
        d.screenshot(os.path.join(a.shots, "history.png"))
        def message_rows():
            """The rows on screen now, oldest first: a row seen a frame ago may have scrolled away."""
            t = targets(d)
            top = t["history-rows"]["y"] + 50 if "history-rows" in t else 0  # clear of the frozen header
            return sorted((k for k in t if k.startswith("history-message:") and t[k]["y"] >= top),
                          key=lambda k: int(k.split(":")[1]))
        rows = until("message rows", message_rows)
        print("[jump] rows in view:", len(rows))
        # an old Message, off the chat's screen: scroll HISTORY back until the first ones show
        # Message rows are sparse among the Field rows, so a page of history may show none
        for _ in range(200):
            seen = message_rows()
            if seen and int(seen[0].split(":")[1]) < first_seq(d):
                break
            d.scroll("history-rows", 4)
            settle(d, 2)
        settle(d, 4)
        before = d.state().get("last_jump")
        def landed():
            return d.state().get("last_jump") not in ("", None, before)
        # older pages arrive while HISTORY scrolls, and move what is under the pointer: look again each try
        for attempt in range(5):
            settle(d, 8)
            rows = message_rows()
            if not rows:
                d.scroll("history-rows", 2)
                continue
            print("[jump] clicking", rows[0], targets(d)[rows[0]])
            d.click(rows[0])
            settle(d, 6)
            try:
                until("the jump to land", landed, 4.0)
                break
            except AssertionError:
                print("[jump] no jump after clicking", rows[0], "try", attempt)
        until("the jump to land", landed, 2.0)
        settle(d)
        d.screenshot(os.path.join(a.shots, "landed.png"))
        if "input" not in targets(d):
            raise AssertionError("chat did not come forward")
        print("[jump] landed on", d.state().get("last_jump"))
        # and back: right click the landed Message, SHOW IN HISTORY
        t = targets(d)["input"]
        print("[jump] input rect", t)
        tx, ty = (t["x"], t["y"]) if isinstance(t, dict) else (t[0], t[1])
        d.call("click", x=tx + 120, y=ty - 180, button="right")
        settle(d, 8)
        until("the menu", lambda: "menu:show-in-history" in targets(d), 5.0)
        d.screenshot(os.path.join(a.shots, "menu.png"))
        d.click("menu:show-in-history")
        settle(d, 10)
        d.screenshot(os.path.join(a.shots, "back.png"))
        if d.state().get("attached") is not True:
            raise AssertionError("lost the world")
        print("[jump] ok")
        return 0
    except Exception as e:
        print("[jump] FAIL", repr(e))
        try:
            d.screenshot(os.path.join(a.shots, "fail.png"))
        except Exception:
            pass
        return 1
    finally:
        try:
            d.call("quit")
        except Exception:
            pass
        time.sleep(0.5)
        proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
