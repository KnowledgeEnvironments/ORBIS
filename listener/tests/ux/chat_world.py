#!/usr/bin/env python3
"""One door per agent, a person's own page, inspectors that read as inspectors.

    privateworld.py --port 7838 -- chat_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>

Nothing here reaches the main world: privateworld refuses its port. Exit 0 on pass, 1 on failure.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
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


def has_input(d):
    return "input" in (d.state().get("targets") or {})


def front(d):
    wins = {w["name"]: w for w in d.state()["windows"]}
    return [n for n in ("CHAT", "GRAPH", "PANES") if n in wins and not wins[n].get("hidden")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-c-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "chat.log"), "w"))
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("the fold", lambda: "principal:person" in d.targets() and "principal:root" in d.targets(), 60.0)
        settle(d, 10)

        # nothing opens by itself: a person lands on who is here
        assert d.state()["principal"] == "", "a tab opened on attach: " + str(d.state()["principal"])
        assert not any(t.startswith("tab:person") for t in d.targets()), "a person tab on attach"
        d.screenshot(os.path.join(shots, "landing.png"))

        # yourself is a page: no line to type on
        d.click("principal:person")
        settle(d, 8)
        t = d.targets()
        assert "tab:you" in t and "tab:messages" in t and "tab:devices" in t, t
        assert not has_input(d), "a person's own page has an input line"
        d.screenshot(os.path.join(shots, "self.png"))
        d.click("tab:messages")
        settle(d)
        assert not has_input(d), "a person's own messages have an input line"
        d.click("tab:you")
        settle(d)

        # an agent is one click from its chat, and the line needs no explaining
        d.click("principal:root")
        settle(d, 8)
        assert "tab:transcript" in d.targets() and has_input(d), d.targets()
        assert "line-label" not in d.targets(), "the line explains TO and ROUTE when there is no choice to make"
        d.click("input")
        d.type("hello from the chat test")
        d.key("Return")
        until("root's reply", lambda: "hello from the chat test" in json.dumps(d.state().get("transcript") or []) and
              len(d.state().get("transcript") or []) >= 2, 60.0)
        d.screenshot(os.path.join(shots, "chat.png"))

        # what agents said to you is on your page
        d.click("tab:person")
        until("the reply on your page", lambda: any(x.startswith("self-message:") for x in d.targets()), 20.0)
        d.screenshot(os.path.join(shots, "self-after.png"))
        d.click(next(x for x in d.targets() if x.startswith("self-message:")))
        settle(d, 8)
        assert has_input(d), "a row on your page opens that agent's chat"

        # inspectors are their own tabs, from a right click on any row of the object list
        d.call("click", target="principal:grid", button="right")
        settle(d)
        d.click("menu:inspect")
        settle(d, 8)
        assert "tab:inspect:grid" in d.targets(), d.targets()
        assert "tab:inspector" in d.targets(), "an inspector is a view of its own, beside the chat"
        d.screenshot(os.path.join(shots, "inspector.png"))
        d.click("principal:grid")
        settle(d, 8)
        assert "tab:grid" in d.targets() and "tab:inspect:grid" in d.targets(), "chat and inspector are two tabs"

        # HISTORY shows what happened, not what was looked at, until asked; Escape leaves its search
        d.click("button:history-expand") if "button:history-expand" in d.targets() else None
        settle(d, 8)
        d.click("button:search")
        settle(d)
        assert "input:search" in d.targets(), "no search box"
        d.key("Escape")
        settle(d)
        assert "input:search" not in d.targets(), "Escape left the search box open"
        d.click("button:history-reads")
        settle(d)
        d.click("button:history-reads")
        settle(d)
        d.click("button:history-collapse")
        settle(d, 8)

        # a pane takes a pretty name from its tab, and the name is on the tab bar and the map
        d.click("tab:panes")
        settle(d)
        d.pane_open(kind="terminal", target="grid.shell")
        until("a terminal pane", lambda: [p for p in d.panes()["live"] if p.get("kind") == "terminal"])
        settle(d, 8)
        tab = next(x for x in d.targets() if x.startswith("tab:") and x not in ("tab:chat", "tab:graph", "tab:panes", "tab:principals", "tab:history", "tab:inspector") and
                   not x.startswith(("tab:person", "tab:root", "tab:grid", "tab:inspect", "tab:transcript", "tab:conv", "tab:instr",
                                     "tab:fields", "tab:memory", "tab:devices", "tab:rules", "tab:you", "tab:messages")))
        d.call("click", target=tab, button="right")
        settle(d)
        d.click("menu:rename-pane")
        settle(d, 8)
        d.click("input:label")
        d.type("my build shell")
        d.click("button:label-set")
        until("the name on its tab", lambda: "tab:my build shell" in d.targets(), 15.0)
        d.screenshot(os.path.join(shots, "pane-named.png"))
        d.click("tab:graph")
        settle(d, 8)
        d.click("menu:view")
        settle(d)
        d.click("menu:twilight-map")
        settle(d, 8)
        d.screenshot(os.path.join(shots, "twilight.png"))
        assert json.load(open(os.path.join(tmp, "data", "prefs.json")))["view"]["map-twilight"] is True, "twilight did not stick"
        d.click("menu:view")
        settle(d)
        d.click("menu:twilight-map")
        settle(d, 6)
        assert any("my build shell" in json.dumps(x) for x in d.state()["map"]["actors"]), "the map does not show the pane's name"
        d.click("tab:chat")
        settle(d, 8)

        # the left column comes back at the width it had
        width = lambda: next(w for w in d.state()["windows"] if w["name"] == "PRINCIPALS")["w"]  # noqa: E731
        before = width()
        for _ in range(2):
            d.click("menu:view")
            settle(d)
            d.click("menu:left-column")
            settle(d, 12)
        until("PRINCIPALS back", lambda: abs(width() - before) <= 2.0, 6.0)

        # the last tab closed goes back to the map
        for _ in range(8):
            d.click("tab:chat")  # closing a pane brings its neighbour forward
            settle(d, 6)
            closes = [x for x in d.targets() if x.startswith("close:") and not x.startswith("close:pane") and
                      x not in ("close:chat", "close:graph", "close:panes", "close:principals", "close:history", "close:inspector", "close:my build shell")]
            if not closes:
                break
            name = closes[0].split(":", 1)[1]
            if "tab:" + name in d.targets():
                d.click("tab:" + name)  # the x of the tab in front
                settle(d, 4)
            d.click(closes[0])
            settle(d, 6)
        until("the map in front", lambda: "button:welcome" in d.targets() or front(d)[:1] == ["GRAPH"] or d.state()["principal"] == "", 10.0)
        print("[chat] ok")
        return 0
    except Exception as e:  # noqa: BLE001
        print("[chat] FAIL", repr(e))
        try:
            d.screenshot(os.path.join(shots, "fail.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        try:
            d.quit()
        except Exception:  # noqa: BLE001
            pass
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.terminate()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
