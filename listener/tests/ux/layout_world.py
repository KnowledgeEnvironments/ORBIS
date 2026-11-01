#!/usr/bin/env python3
"""One tree of views and panes, saved on the person.

    privateworld.py --port 7846 -- layout_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

History dragged beside Chat comes back there after a restart; a closed view reopens from VIEW; a
view floats and docks; RESET LAYOUT puts things back; moving between a chat and an inspector moves
nothing. Nothing here reaches the main world: privateworld refuses its port.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=5000)


def until(what, check, seconds=30.0):
    end = time.time() + seconds
    while time.time() < end:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError("never: " + what)


def layout(d):
    return d.call("panes")["panes"]


def front(d, view):
    for v in layout(d).get("front") or []:
        if v["view"] == view:
            return v
    return None


def leaf_tabs(node, out=None):
    out = [] if out is None else out
    if "tabs" in node:
        out.append(node["tabs"])
    for k in ("a", "b"):
        if k in node:
            leaf_tabs(node[k], out)
    return out


def rects(d, names):
    t = d.state()["targets"]
    return {n: t.get(n) for n in names}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-l-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    log = open(os.path.join(shots, "layout.log"), "w")

    def start():
        p = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                              "--data-dir", os.path.join(tmp, "data")], stdout=subprocess.DEVNULL, stderr=log)
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("the fold", lambda: "principal:root" in d.targets(), 60.0)
        settle(d, 10)
        return p, d

    proc, d = start()
    try:
        # the map is the landing, in a tree that holds every view
        tabs = leaf_tabs(layout(d)["layout"])
        assert ["view:principals"] in tabs and ["view:chat", "view:graph", "view:panes"] in tabs, tabs

        # a chat and an inspector: neither move anything
        d.click("principal:root")
        settle(d, 8)
        d.call("click", target="principal:grid", button="right")
        settle(d)
        d.click("menu:inspect")
        settle(d, 8)
        watch = ["tab:chat", "tab:panes", "tab:principals"]
        before = rects(d, watch)
        for name in ("tab:inspect:grid", "tab:root", "tab:inspect:grid", "tab:root"):
            d.click(name)
            settle(d, 4)
            assert rects(d, watch) == before, (name, before, rects(d, watch))
        d.screenshot(os.path.join(shots, "chat-and-inspector.png"))
        d.click("close:inspect:grid") if "close:inspect:grid" in d.targets() else None
        settle(d, 8)
        assert "tab:inspector" not in d.targets(), "the last inspector closes its view"

        # the x of a tab behind: a person's pointer rests on it first, the scroll stands in for that
        d.click("principal:grid")
        settle(d, 8)
        d.click("principal:root")
        settle(d, 8)
        d.scroll("tab:grid", 0.01)
        settle(d, 6)
        d.click("close:grid")
        settle(d, 8)
        assert "tab:grid" not in d.targets(), "the tab behind closes"

        # History beside Chat
        d.click("button:history-expand") if "button:history-expand" in d.targets() else None
        settle(d, 8)
        d.click("tab:chat")
        settle(d, 6)
        chat = front(d, "view:chat")
        d.call("drag", **{"from": "tab:history", "to_x": chat["x"] + chat["w"] * 0.92, "to_y": chat["y"] + chat["h"] * 0.5})
        settle(d, 10)
        chat, hist = front(d, "view:chat"), front(d, "view:history")
        assert hist and chat and hist["x"] > chat["x"] + chat["w"] - 4, (chat, hist)
        assert abs(hist["y"] - chat["y"]) < 4, (chat, hist)
        d.screenshot(os.path.join(shots, "history-beside-chat.png"))
        time.sleep(1.5)  # the Field is written a moment after the last move
        settle(d, 4)

        # it comes back
        d.call("quit")
        proc.wait(timeout=15)
        proc, d = start()
        until("the saved layout", lambda: front(d, "view:history") is not None and front(d, "view:chat") is not None
              or any("view:history" in t for t in leaf_tabs(layout(d)["layout"])), 20.0)
        d.click("tab:chat")
        settle(d, 8)
        chat, hist = front(d, "view:chat"), front(d, "view:history")
        assert hist and chat and hist["x"] > chat["x"] + chat["w"] - 4 and abs(hist["y"] - chat["y"]) < 4, (chat, hist)
        d.screenshot(os.path.join(shots, "after-restart.png"))

        # a closed view reopens from VIEW
        d.click("tab:graph")
        settle(d, 6)
        d.click("close:graph")
        settle(d, 8)
        assert "tab:graph" not in d.targets() and not any("view:graph" in t for t in leaf_tabs(layout(d)["layout"]))
        d.click("menu:view")
        settle(d)
        d.click("menu:graph")
        settle(d, 8)
        assert "tab:graph" in d.targets() and front(d, "view:graph"), "VIEW brings it back"

        # a view floats, and docks when its window closes
        leaf = front(d, "view:history")
        d.call("click", target="tab:history", button="right")
        settle(d)
        d.click("menu:float-pane")
        settle(d, 10)
        floats = layout(d)["layout"].get("floats") or []
        assert [f["pane"] for f in floats] == ["view:history"], floats
        assert "tab:history" not in d.targets()
        d.screenshot(os.path.join(shots, "floating.png"))
        r = floats[0]
        assert r["w"] < leaf["w"] and r["x"] > leaf["x"], "out of its leaf, smaller and offset: " + str((r, leaf))
        at = d.state()["targets"]["button:history-collapse"]
        assert r["x"] <= at["x"] and at["x"] + at["w"] <= r["x"] + r["w"], (r, at)
        assert r["y"] <= at["y"] and at["y"] + at["h"] <= r["y"] + r["h"], (r, at)
        d.click("button:history-collapse")  # lands only if the window is on top of the tree
        settle(d, 6)
        assert "button:history-expand" in d.targets(), "the floating window took the click"
        d.click("button:history-expand")
        settle(d, 6)

        # back to the start
        d.click("menu:view")
        settle(d)
        d.click("menu:reset-layout")
        settle(d, 10)
        tabs = leaf_tabs(layout(d)["layout"])
        assert ["view:principals"] in tabs and ["view:history"] in tabs, tabs
        assert ["view:chat", "view:graph", "view:panes"] in tabs, tabs
        assert not layout(d)["layout"].get("floats")
        assert "button:history-expand" in d.targets(), "History folds again"
        d.screenshot(os.path.join(shots, "reset.png"))
        d.call("quit")
        proc.wait(timeout=15)
        print("[layout] ok")
        return 0
    except Exception as e:  # noqa: BLE001
        print("[layout] FAIL", repr(e)[:800])
        try:
            d.screenshot(os.path.join(shots, "fail.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
