#!/usr/bin/env python3
"""The manual, edits a Principal hears about, compression, and leaving the box, on a real world.

    privateworld.py --port 7833 -- manual_world.py --listener <binary> --url {url} --token {token}
                                                   --root-token {root} --shots <dir>

Exit 0 on pass, 1 on failure.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drive import Drive  # noqa: E402
from privateworld import Client  # noqa: E402


def until(what, fn, seconds=20.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.15)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root-token", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-m-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "listener.log"), "w"))
    port = urlparse(a.url).port
    person = Client(port, open(a.token_file).read().strip())
    other = Client(port, open(a.root_token).read().strip())

    def settle(frames=6):
        d.wait("settle", frames=frames, timeout_ms=8000)

    def manual():
        return d.state().get("manual", {})

    def shot(name):
        settle(8)
        d.screenshot(os.path.join(shots, name + ".png"))

    try:
        d = Drive(sock)
        d.wait("attached", timeout_ms=30000)
        d.wait("route", timeout_ms=30000)
        d.resize(1280, 800)
        settle(10)
        root = person.req("resolve", {"name": "root", "op": "read"})["result"]["object"]

        # HELP, SHOW DOCUMENTATION: the books, then a search
        d.click("menu:help")
        settle()
        d.click("menu:show-documentation")
        until("the books", lambda: manual().get("books", 0) > 3)
        shot("manual-open")
        d.click("input:manual-search")
        settle(4)
        d.type("restart")
        rows = until("candidates", lambda: manual().get("candidates"))
        assert "action/world.restart" in rows, rows
        shot("manual-candidates")
        d.click("doc:action/world.restart")
        until("the topic", lambda: manual().get("topic") == "action/world.restart")
        assert manual()["chars"] > 40, manual()
        shot("manual-topic")

        # a bookmark is a Field of the person's, so a second client has it
        d.click("button:manual-bookmark")
        me = person.req("resolve", {"name": "person", "op": "read"})["result"]["object"]
        def marks():
            r = person.req("read", {"object": me, "field": "view/bookmarks"})
            return r.get("ok") and "action/world.restart" in str(r.get("result"))
        until("the bookmark", marks)
        until("its row", lambda: "doc:action/world.restart" in d.targets())

        # reading on, and back
        d.click("input:manual-search")
        settle(4)
        d.key("a", ["super"])
        d.key("Backspace")
        settle(4)
        d.type("resolution")
        until("the LaTeX topic", lambda: "start/resolution" in (manual().get("candidates") or []))
        d.click("doc:start/resolution")
        until("the topic", lambda: manual().get("topic") == "start/resolution")
        assert manual()["format"] == "latex", manual()
        shot("manual-latex")
        d.click("button:manual-back")
        until("back", lambda: manual().get("topic") == "action/world.restart")
        d.click("button:manual-next")
        until("next", lambda: manual().get("topic") != "action/world.restart")
        d.click("menu:help")
        settle()
        d.click("menu:show-documentation")
        until("closed", lambda: not manual().get("open"))

        # an edit by someone else: waiting, a count on the tab, seen after a wake
        d.click("tab:chat")
        settle()
        d.click("principal:root")
        settle(8)
        d.click("input")
        settle(4)
        d.type("first wake")
        settle(4)
        d.key("Return")
        d.wait("reply", timeout_ms=60000)
        until("the seen mark", lambda: person.req("read", {"object": root, "field": "loop/seen-seq"}).get("ok"))
        d.click("tab:instructions")
        settle(10)
        r = person.req("set-field", {"object": root, "field": "instructions/charter", "type": "string",
                                     "value": "You answer briefly.\n\nA second paragraph."})
        assert r.get("ok"), r
        target = "news:instructions/charter"
        until("applies at the next wake", lambda: target in d.targets())
        shot("edit-waits")
        d.click("tab:transcript")
        settle(4)
        d.click("input")
        settle(4)
        d.type("second wake")
        settle(4)
        d.key("Return")
        d.wait("reply", timeout_ms=60000)
        told = open(os.path.join(os.environ["ORBIS_WORLD_DATA"], "last-prompt.json")).read()
        assert "instructions/charter" in told and "Changed since your last wake" in told, told[:400]
        d.click("tab:instructions")
        settle(10)
        shot("edit-seen")
        diff = person.req("act", {"action": "field.diff", "args": {"object": root, "field": "instructions/charter"}})
        assert "+ You answer briefly." in str(diff), diff

        # compression: the three together
        d.click("tab:conversations")
        settle(10)
        d.click("input:serialize/summary-tokens")
        settle(4)
        d.type("250")
        settle(4)
        d.click("button:set:serialize/summary-tokens")
        until("the size", lambda: "250" in str(person.req("read", {"object": root, "field": "serialize/summary-tokens"})))
        shot("compression")
        d.click("button:compressor")
        def compressor():
            return next((p for p in d.panes().get("live", []) if p.get("field") == "serialize/compressor"), None)
        pane = until("the prompt in the editor", compressor)
        assert "conversation" in pane["text"], pane
        settle(6)

        # OPEN FILE: a mount, a path that completes, Tab, the arrows, Enter
        d.click("menu:file")
        settle()
        d.click("menu:open-file")
        settle(8)
        assert "button:mount:grid.fs" in d.targets(), d.targets()
        d.click("button:mount:grid.fs")
        settle(6)
        def opened():
            return d.state().get("open_file", {})
        entries = until("the mount's top level", lambda: opened().get("entries"))
        assert any(e.endswith("/") for e in entries) or entries, entries
        first = next((e for e in entries if not e.endswith("/")), entries[0])
        d.type(first[:2])
        until("narrowed", lambda: all(e.startswith(first[:2]) for e in opened().get("entries", [""])) and opened().get("entries"))
        shot("open-file")
        d.key("Tab")
        until("Tab extended the line", lambda: len(opened().get("path", "")) > 2)
        d.key("Down")
        until("a candidate is picked", lambda: opened().get("selected", -1) >= 0)
        d.key("Return")
        until("it opened in a pane", lambda: any(p.get("title") and first.rstrip("/") in str(p) for p in d.panes().get("live", [])))
        settle(6)
        # a refused open reaches the modeline, wherever the person is looking
        d.click("menu:file")
        settle()
        d.click("menu:open-file")
        settle(6)
        d.click("button:mount:grid.fs")
        settle(6)
        d.type("no/such/file.txt")
        settle(6)
        d.key("Return")
        told = until("a refusal in the modeline", lambda: d.state().get("modeline_error"))
        assert "err" in told, told
        shot("modeline-refusal")
        settle(6)
        d.click("menu:file")
        settle()
        d.click("menu:open-file")
        settle(6)
        d.key("Escape")
        settle(6)
        assert "input:open-file" not in d.targets(), "Escape cancels"

        # leaving the box: a confirm that says what it opens, then the red indicator, then back
        shell = person.req("resolve", {"name": "grid.shell", "op": "read"})["result"]["object"]
        d.click("tab:chat")
        settle(6)
        d.click("button:tree-flat")
        settle(6)
        d.click("principal:grid.shell")
        settle(10)
        d.click(f"button:unconfine:{shell}")
        settle(6)
        shot("box-confirm")
        d.click("button:box-30")
        until("the red indicator", lambda: "unsafe" in d.targets())
        shot("box-out")
        d.click("unsafe")
        settle(6)
        d.click("button:unsafe-off:grid.shell")
        until("back in the box", lambda: "unsafe" not in d.targets())
        print("[manual] ok")
        return 0
    except Exception as e:
        print(f"[manual] FAIL {type(e).__name__}: {e}")
        try:
            d.screenshot(os.path.join(shots, "failure.png"))
            print([t for t in d.targets() if not t.startswith(("menu:", "tab:"))][:60])
        except Exception:
            pass
        return 1
    finally:
        try:
            d.quit()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
