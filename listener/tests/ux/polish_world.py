#!/usr/bin/env python3
"""The language of controls, on a real world: every add is a small `+`, wells are inset, and the tab row
stays put when a chat gives way to an inspector.

    privateworld.py --port 7836 -- python3 polish_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

Exit 0 on pass, 1 on failure.
"""
import argparse
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
    while time.time() < end:
        if check():
            return
        time.sleep(0.08)
    raise AssertionError("timed out waiting for " + what)


def rect(d, name):
    return d.state()["targets"][name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-p-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "polish.log"), "w"))
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("the fold", lambda: "principal:root" in d.targets() and "principal:shell.local" in d.targets() or
              "principal:root" in d.targets(), 60.0)
        settle(d, 10)
        d.screenshot(os.path.join(shots, "landing.png"))
        front = [w["name"] for w in d.state()["windows"] if w.get("front") and w["name"] in ("CHAT", "GRAPH", "PANES")]
        assert front == ["GRAPH"], front
        d.click("principal:root")
        settle(d, 8)
        d.click("input")
        d.type("hello from the polish test")
        d.key("Return")
        until("root's reply", lambda: len(d.state().get("transcript") or []) >= 2, 60.0)
        settle(d, 10)
        before = rect(d, "tab:chat")
        d.screenshot(os.path.join(shots, "chat.png"))
        t = d.state()["targets"]
        assert t["button:attach-file"]["w"] == t["button:new-principal"]["w"], "every add is the same [+]"
        assert "button:new-principal" in t and t["button:new-principal"]["w"] < 24, t["button:new-principal"]
        d.click("button:tree-flat")
        settle(d, 6)
        d.click("principal:shell.local")
        settle(d, 10)
        assert rect(d, "tab:chat") == before, "the tab row moved"
        d.screenshot(os.path.join(shots, "inspector.png"))
        badges = sorted((v["x"], k) for k, v in d.state()["targets"].items() if k.startswith("button:comment:"))
        assert badges and len({x for x, _ in badges}) == 1, badges
        d.click("tab:root")
        settle(d, 8)
        assert rect(d, "tab:chat") == before, "the tab row moved back"
        for tab, name in (("tab:instructions", "instructions"), ("tab:memory", "memory"), ("tab:fields", "fields"),
                          ("tab:conversations", "conversations"), ("tab:devices", "devices"), ("tab:rules", "rules")):
            d.click(tab)
            settle(d, 8)
            d.screenshot(os.path.join(shots, name + ".png"))
        d.click("tab:transcript")
        settle(d, 6)
        d.call("click", target="principal:root", button="right")
        settle(d)
        d.key("Escape")
        settle(d)
        d.click("button:model")
        settle(d, 6)
        d.screenshot(os.path.join(shots, "models.png"))
        assert any(k.startswith("menu:model:") for k in d.targets()), "the switcher lists the Models"
        assert "menu:model-settings" in d.targets(), "settings are one level in"
        d.key("Escape")
        settle(d)
        d.click("input")
        d.key("m", mods=["ctrl"])
        settle(d, 6)
        assert "menu:model-settings" in d.targets(), "Ctrl-M opens the switcher"
        d.key("Escape")
        settle(d)
        d.click("button:history-expand")
        settle(d, 8)
        d.click("tab:chat")
        settle(d, 6)
        d.screenshot(os.path.join(shots, "history.png"))
        d.click("button:history-collapse")
        settle(d, 6)
        d.click("tab:panes")
        settle(d, 6)
        d.click("button:new-pane")
        settle(d, 6)
        d.screenshot(os.path.join(shots, "newpane.png"))
        d.click("menu:terminal-on-grid.shell")
        settle(d, 12)
        tab = next(x for x in d.targets() if x.startswith("tab:") and x not in ("tab:chat", "tab:graph", "tab:panes", "tab:principals", "tab:history", "tab:inspector") and
                   not x.startswith(("tab:root", "tab:shell", "tab:transcript", "tab:conversations", "tab:instructions",
                                     "tab:fields", "tab:memory", "tab:devices", "tab:rules", "tab:inspect")))
        d.call("click", target=tab, button="right")
        settle(d)
        d.click("menu:rename-pane")
        settle(d, 8)
        d.screenshot(os.path.join(shots, "rename.png"))
        d.key("Escape")
        settle(d)
        d.click("tab:panes")
        settle(d, 6)
        d.screenshot(os.path.join(shots, "panes.png"))
        print("[polish] ok")
        return 0
    finally:
        try:
            d.quit()
        except Exception:
            proc.terminate()
        proc.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
