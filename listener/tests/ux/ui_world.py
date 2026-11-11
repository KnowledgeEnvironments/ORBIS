#!/usr/bin/env python3
"""What a person does around the panes, on a real world: unsafe mode from the confirm to the red
indicator on two clients, APPROVE ALL, every menu item, Escape on every dialog, the tree, nicknames.

    privateworld.py --port 7826 -- ui_world.py --listener <binary> --url {url} --token-file {token}

Exit 0 on pass, 1 on failure.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
from drive import Drive  # noqa: E402
from privateworld import Client  # noqa: E402

TOPS = ("menu:file", "menu:edit", "menu:view", "menu:world", "menu:help")
# Everything but what would end the run or needs a selection: each is clicked and the client lives.
WALK = {
    "file": ["new-pane", "open-file"],
    "edit": ["find"],
    "view": ["graph", "panes", "chat", "twilight-map", "twilight-map", "history", "history", "left-column", "left-column", "show-reasoning",
             "show-reasoning", "text-larger", "text-smaller", "text-default"],
    "world": ["approvals", "approvals", "rules", "conversations", "devices", "pair-a-device", "compress", "switch-world"],
    "help": ["keyboard-shortcuts", "what-things-are", "about"],
}
DIALOGS = {"open-file", "keyboard-shortcuts", "what-things-are", "about", "switch-world"}


def until(what, fn, seconds=60.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.15)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=30000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-u-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    procs = []

    def client(name):
        home = os.path.join(tmp, name)
        os.makedirs(home)
        sock = os.path.join(home, "d.sock")
        p = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                              "--data-dir", os.path.join(home, "data")],
                             stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, name + ".log"), "w"))
        procs.append(p)
        drv = Drive(sock)
        drv.wait("attached", timeout_ms=30000)
        drv.resize(1280, 800)
        settle(drv, 10)
        return drv

    port = urlparse(a.url).port
    c = Client(port, open(a.token_file).read().strip())
    name_id = lambda n: c.req("resolve", {"name": n, "op": "read"})["result"]["object"]  # noqa: E731
    d = client("first")
    try:
        root, grid = name_id("root"), name_id("grid")

        # NEW PANE offers what each Adapter can be
        d.click("tab:panes")
        settle(d)
        d.click("button:new-pane")
        settle(d)
        offered = {t for t in d.targets() if t.startswith("menu:") and t not in TOPS}
        assert "menu:terminal-on-grid.shell" in offered and "menu:files-on-grid.fs" in offered, offered
        assert not offered & {"menu:files-on-grid.shell", "menu:terminal-on-grid.fs", "menu:terminal-on-transport.listener"}, offered
        d.key("Escape")
        settle(d, 4)
        d.click("tab:chat")
        settle(d)

        # unsafe mode: confirm, arm, indicator, every dock, a second client, a run with no card, off
        d.click("principal:grid")
        settle(d, 8)
        d.click("button:actions")
        settle(d)
        d.click("button:unsafe:" + grid)
        settle(d)
        d.screenshot(os.path.join(shots, "unsafe-confirm.png"))
        d.key("Escape")
        settle(d)
        assert "unsafe" not in d.targets() and "button:unsafe-arm" not in d.targets(), "Escape armed or stayed"
        d.click("button:actions")
        settle(d)
        d.click("button:unsafe:" + grid)
        settle(d)
        d.click("button:unsafe-minutes:30")
        settle(d, 4)
        d.click("button:unsafe-arm")
        until("the indicator", lambda: "unsafe" in d.targets(), 60.0)
        for dock in ("tab:panes", "tab:graph", "tab:chat"):
            d.click(dock)
            settle(d)
            assert "unsafe" in d.targets(), "no indicator on " + dock
        d.screenshot(os.path.join(shots, "unsafe-armed.png"))
        b = client("second")
        until("the indicator on a client that attached after", lambda: "unsafe" in b.targets(), 60.0)
        route = name_id("root.grid")
        op = {"op": "act", "args": {"action": "task.run", "args": {"adapter": "grid.shell", "cmd": "echo under-unsafe"}}}
        c.req("send", {"to": [grid], "route": route, "text": "ops " + json.dumps(op)})
        until("a run with no card", lambda: "under-unsafe" in json.dumps(d.state().get("transcript") or []), 60.0)
        assert not d.state().get("tasks"), d.state().get("tasks")
        d.click("button:approvals")
        settle(d, 8)
        d.screenshot(os.path.join(shots, "ran-under-unsafe.png"))
        d.click("button:approvals")
        settle(d)
        d.click("unsafe")
        settle(d)
        d.screenshot(os.path.join(shots, "unsafe-list.png"))
        d.click("button:unsafe-off:grid")
        until("off here", lambda: "unsafe" not in d.targets(), 60.0)
        until("off on the other client", lambda: "unsafe" not in b.targets(), 60.0)
        b.quit()
        print("[ui] unsafe ok")

        # APPROVE ALL: three cards, a confirm that Escape refuses, a confirm that takes them
        rroute = name_id("root.person")
        for i in range(3):
            c.req("send", {"to": [root], "route": rroute, "text": f"run exactly this command: echo all-{i}."})
            until(f"card {i + 1}", lambda: len(d.state().get("tasks") or []) > i, 60.0)
        until("three cards", lambda: len(d.state().get("tasks") or []) >= 3, 60.0)
        d.click("button:approvals")
        settle(d, 8)
        d.screenshot(os.path.join(shots, "approvals.png"))
        d.click("button:approve-all")
        settle(d)
        d.key("Escape")
        settle(d)
        assert len(d.state().get("tasks") or []) >= 3, "Escape decided"
        d.click("button:approve-all")
        settle(d)
        d.screenshot(os.path.join(shots, "approve-all.png"))
        d.click("button:approve-all-yes")
        until("all decided", lambda: not d.state().get("tasks"), 60.0)
        d.click("button:approvals")
        settle(d)
        print("[ui] approve all ok")

        # every menu item
        d.click("principal:root")
        settle(d, 8)
        for menu, items in WALK.items():
            for item in items:
                d.click("menu:" + menu)
                settle(d, 4)
                assert "menu:" + item in d.targets(), f"{menu} has no {item}"
                d.click("menu:" + item)
                settle(d, 8)
                if item in DIALOGS:
                    d.screenshot(os.path.join(shots, "menu-" + item + ".png"))
                    d.key("Escape")
                    settle(d)
        print("[ui] menus ok")

        # an @ mention goes out as a structured reference
        d.click("tab:chat")
        settle(d)
        d.click("principal:root")
        settle(d)
        d.click("tab:transcript")
        settle(d)
        d.click("input")
        settle(d, 4)
        def mine():
            return [l for l in d.state()["transcript"] if l.get("mine")]

        for how in ("key", "click"):
            sent = len(mine())
            d.click("input")
            settle(d, 4)
            d.type(f"look by {how} at @gri")
            until("the @ list", lambda: "at:grid" in d.targets(), 60.0)
            if how == "key":
                d.key("Tab")
            else:
                d.click("at:grid")
            settle(d)
            line = d.state()["input_text"]
            assert line.endswith("grid "), (how, line)
            d.key("Return")
            until("the message", lambda: len(mine()) > sent, 60.0)
            message = mine()[-1]["message"]
            fields = c.req("read", {"object": message})["result"]["object/fields"]
            assert grid in json.dumps(fields["message/refs"]["field/value"]), fields.get("message/refs")
            settle(d, 8)
        print("[ui] mention ok")

        # the tree and a nickname
        d.click("tree-toggle:grid")
        settle(d)
        assert "tree:grid:grid.shell" in d.targets() and "tree:grid:grid.fs" in d.targets()
        d.click("principal:grid", times=2)
        settle(d, 8)
        d.click("input:label")
        settle(d, 3)
        d.type("Grid Project")
        d.key("Return")
        until("the nickname in the tree", lambda: "principal:grid project" in d.targets(), 60.0)
        fields = c.req("read", {"object": grid, "field": "object/label"})["result"]["object/fields"]
        assert fields["object/label"]["field/value"] == "Grid Project", fields
        d.screenshot(os.path.join(shots, "tree.png"))
        print("[ui] pass; screenshots in", shots)
        return 0
    except Exception as e:  # noqa: BLE001
        print("[ui] FAIL", repr(e))
        try:
            d.screenshot(os.path.join(shots, "failure.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        c.close()
        try:
            d.quit()
        except Exception:  # noqa: BLE001
            pass
        for p in procs:
            p.terminate()


if __name__ == "__main__":
    sys.exit(main())
