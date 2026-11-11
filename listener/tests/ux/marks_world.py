#!/usr/bin/env python3
"""Activity marks: working, needs you, finished unseen on a second client, cleared by looking, and
a chain of agents giving one mark at the end.

    privateworld.py --port 7836 -- marks_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

Nothing here reaches the main world: privateworld refuses its port. Exit 0 on pass, 1 on failure.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=15000)


def until(what, check, seconds=30.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = check()
        if last:
            return last
        time.sleep(0.08)
    raise AssertionError("timed out waiting for " + what)


def marks(d):
    return (d.state().get("activity") or {}).get("marks") or {}


def rollup(d):
    return (d.state().get("activity") or {}).get("rollup") or ""


def client(listener, url, token, tmp, name, shots):
    sock = os.path.join(tmp, name + ".sock")
    proc = subprocess.Popen([listener, "--url", url, "--token-file", token, "--drive", sock,
                             "--data-dir", os.path.join(tmp, name)],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, name + ".log"), "w"))
    d = Drive(sock, connect_timeout=30.0)
    d.wait("attached", timeout_ms=30000)
    d.resize(1280, 800)
    until("the fold", lambda: "principal:root" in d.targets(), 60.0)
    settle(d, 10)
    return proc, d


def open_root(d):
    time.sleep(0.6)  # two clicks in a row would be a double click, which renames
    d.click("principal:root")
    settle(d, 8)


def say(d, text):
    open_root(d)
    d.click("input")
    d.type(text)
    d.key("Return")
    settle(d)


def away(d):
    d.click("window:graph" if "window:graph" in d.targets() else "tab:graph")
    settle(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-m-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    procs = []
    try:
        p, one = client(a.listener, a.url, a.token_file, tmp, "one", shots)
        procs.append(p)

        # an agent starts plain: whatever it said at boot is cleared by opening it
        open_root(one)
        until("a clear mark", lambda: marks(one).get("root") == "idle", 20.0)

        # working
        say(one, "Count slowly")
        until("working", lambda: marks(one).get("root") == "working", 20.0)
        one.screenshot(os.path.join(shots, "working.png"))

        # finished, unseen: the person left while it worked
        away(one)
        until("finished", lambda: marks(one).get("root") == "finished, not seen", 60.0)
        assert rollup(one) == "1 done", rollup(one)
        one.screenshot(os.path.join(shots, "finished.png"))

        # a second client of the same person agrees, because the look is in the world
        p, two = client(a.listener, a.url, a.token_file, tmp, "two", shots)
        procs.append(p)
        until("finished on the second client", lambda: marks(two).get("root") == "finished, not seen", 20.0)
        two.screenshot(os.path.join(shots, "finished-second.png"))

        # the modeline goes to it, and looking clears it on both
        one.click("modeline:next")
        until("cleared by looking", lambda: marks(one).get("root") == "idle", 20.0)
        until("cleared on the second client", lambda: marks(two).get("root") == "idle", 20.0)
        assert rollup(two) == "", rollup(two)
        two.screenshot(os.path.join(shots, "cleared-second.png"))
        procs.pop().terminate()

        # needs you: a card, however the person is looking
        say(one, "run exactly this command: ls.")
        away(one)
        until("needs you", lambda: marks(one).get("root") == "needs you", 30.0)
        assert "need" in rollup(one), rollup(one)
        one.screenshot(os.path.join(shots, "needs-you.png"))

        # a chain: a subagent answers its parent, the person hears once, from the parent
        one.click("button:approve:root")
        until("the card is gone", lambda: marks(one).get("root") != "needs you", 30.0)
        open_root(one)
        until("seen", lambda: marks(one).get("root") in ("idle", "working"), 30.0)
        say(one, 'ops {"op":"act","args":{"action":"delegate","args":{"task":"Say done, in one line."}}}')
        away(one)
        until("the delegation's card", lambda: marks(one).get("root") == "needs you", 30.0)
        one.click("button:approve:root")

        def chain_done():
            m = marks(one)
            return m.get("root") == "finished, not seen"

        until("the parent finished", chain_done, 60.0)
        m = marks(one)
        others = {k: v for k, v in m.items() if k not in ("root", "grid")}
        assert all(v != "finished, not seen" for v in others.values()), m
        assert rollup(one) == "1 done", rollup(one)
        print("[marks] chain", json.dumps(m))
        one.screenshot(os.path.join(shots, "chain.png"))
        print("[marks] ok")
        return 0
    except AssertionError as e:
        print("[marks] FAIL", e)
        try:
            one.screenshot(os.path.join(shots, "fail.png"))
            print(json.dumps(one.state().get("activity")))
        except Exception:
            pass
        return 1
    finally:
        for p in procs:
            p.terminate()


if __name__ == "__main__":
    sys.exit(main())
