#!/usr/bin/env python3
"""A long Field, from its row to the next prompt, on a real world.

    privateworld.py --port 7838 -- field_editor.py --listener <binary> --url {url} --token-file {token}
                                                   --root-token {root} --shots <dir>

A system prompt is too long for a table row: its EDIT opens the editor in a tab. Typing and saving
changes the Field and the very next Serialization; a writer elsewhere reloads a clean buffer and
leaves a held one to its holder; revert, wrap and the rendered view work; a short Field keeps its
inline row. Exit 0 on pass, 1 on failure.
"""
import argparse
import json
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

PROMPT = "instructions/charter"
MARKDOWN = "# Notes for the orbit\n\nA first paragraph that runs on for a while so that the row has to fold it, " \
           "and then on again, and again, well past what a table cell can show.\n\n- one\n- two\n"


def until(what, fn, seconds=15.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.15)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=8000)


def editor(d, field):
    for p in d.panes().get("live", []):
        if p.get("field") == field:
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root-token", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-f-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "listener.log"), "w"))
    port = urlparse(a.url).port
    person = Client(port, open(a.token_file).read().strip())
    other = Client(port, open(a.root_token).read().strip())
    field = lambda obj, name: person.req("read", {"object": obj, "field": name})  # noqa: E731

    def value(obj, name):
        r = field(obj, name)
        assert r.get("ok"), r
        return r["result"]

    try:
        d = Drive(sock)
        d.wait("attached", timeout_ms=30000)
        d.wait("route", timeout_ms=30000)
        d.resize(1280, 800)
        settle(d, 10)
        root = person.req("resolve", {"name": "root", "op": "read"})["result"]["object"]
        grid = person.req("resolve", {"name": "grid", "op": "read"})["result"]["object"]
        r = other.req("set-field", {"object": grid, "field": "notes/long", "type": "string", "value": MARKDOWN, "reader": "both"})
        assert r.get("ok"), r
        r = other.req("set-field", {"object": grid, "field": "notes/short", "type": "string", "value": "brief", "reader": "both"})
        assert r.get("ok"), r

        # INSTRUCTIONS: the system prompt folds, says how long it is, and EDIT opens the editor
        d.click("tab:chat")
        settle(d)
        d.click("principal:root")
        settle(d, 8)
        d.click("tab:instructions")
        settle(d, 10)
        d.screenshot(os.path.join(shots, "instructions.png"))
        target = "button:edit:" + PROMPT
        until("the prompt's EDIT", lambda: target in d.targets())
        d.click(target)
        pane = until("an editor on the prompt", lambda: editor(d, PROMPT))
        settle(d, 10)
        assert d.panes()["focus"] == pane["pane"], d.panes()
        assert pane["wrap"] is True, pane
        pane = until("the title", lambda: (lambda p: p if p and "root" in p.get("title", "") and PROMPT in p["title"] else None)(editor(d, PROMPT)))
        assert not pane["dirty"], pane
        before = pane["text"]
        assert pane["rows"] > pane["lines"], f"a long prompt wraps: {pane['rows']} rows, {pane['lines']} lines"
        d.screenshot(os.path.join(shots, "editor-wrapped.png"))

        # two fingers on a trackpad: a stream of fractions of a notch, which a mouse wheel never sends
        for _ in range(20):
            d.scroll("editor-body", -0.1)
        settle(d, 6)
        down = editor(d, PROMPT)["scroll"]
        assert down >= 5, f"small scroll deltas add up: at row {down}"
        for _ in range(20):
            d.scroll("editor-body", 0.1)
        settle(d, 6)
        assert editor(d, PROMPT)["scroll"] < down, "and scroll back"
        d.scroll("editor-body", -2)
        settle(d, 6)
        assert editor(d, PROMPT)["scroll"] > 0, "a wheel's whole notch scrolls too"

        # type, save: the Field, its type and reader, and the next prompt
        d.key("Up", ["super"])
        settle(d, 4)
        d.key("Up", ["super"])
        settle(d, 4)
        d.type("MARKER-FROM-THE-EDITOR ")
        p = until("the edit", lambda: (lambda x: x if x and x["dirty"] and "MARKER-FROM-THE-EDITOR" in x["text"] else None)(editor(d, PROMPT)))
        settle(d, 4)
        d.screenshot(os.path.join(shots, "editor-dirty.png"))
        d.click("button:editor-save")
        until("saved", lambda: not editor(d, PROMPT)["dirty"])
        v = value(root, PROMPT)
        assert "MARKER-FROM-THE-EDITOR" in json.dumps(v), v
        d.click("tab:chat")
        settle(d)
        d.click("tab:transcript")
        settle(d)
        d.click("input")
        settle(d, 4)
        d.type("are you there")
        settle(d, 4)
        d.key("Return")
        d.wait("reply", timeout_ms=60000)
        prompts = os.path.join(os.environ["ORBIS_WORLD_DATA"], "last-prompt.json")
        told = open(prompts).read()
        assert "MARKER-FROM-THE-EDITOR" in told, "the saved prompt did not reach the model"

        # a clean buffer follows a write from elsewhere; a held one is left to its holder
        d.click(next(t for t in d.targets() if t.startswith("tab:") and PROMPT in t))
        settle(d)
        r = other.req("set-field", {"object": root, "field": PROMPT, "type": "string", "value": before + "\nSECOND-WRITER", "reader": "agent"})
        assert r.get("ok"), r
        until("a reload", lambda: "SECOND-WRITER" in editor(d, PROMPT)["text"])
        assert not editor(d, PROMPT)["dirty"]
        d.type("HELD ")
        until("held", lambda: editor(d, PROMPT)["dirty"])
        settle(d, 6)
        r = other.req("set-field", {"object": root, "field": PROMPT, "type": "string", "value": "THIRD-WRITER\n" + before, "reader": "agent"})
        assert r.get("ok"), r
        p = until("stale", lambda: (lambda x: x if x["conflict"] else None)(editor(d, PROMPT)))
        assert "HELD" in p["text"] and "THIRD-WRITER" not in p["text"], p["text"][:200]
        settle(d, 4)
        d.screenshot(os.path.join(shots, "editor-stale.png"))
        d.click("button:editor-revert")
        settle(d, 4)
        assert "button:editor-revert" in d.targets()
        assert "THIRD-WRITER" not in editor(d, PROMPT)["text"], "one press only asks"
        d.click("button:editor-revert")
        until("reverted", lambda: "THIRD-WRITER" in editor(d, PROMPT)["text"])
        p = editor(d, PROMPT)
        assert not p["dirty"] and not p["conflict"] and "HELD" not in p["text"], p

        # wrap off: one row to a line
        d.click("button:editor-wrap")
        settle(d, 6)
        p = editor(d, PROMPT)
        assert p["wrap"] is False and p["rows"] == p["lines"], p
        d.click("button:editor-wrap")
        settle(d, 4)

        # FIELDS: a long Field folds with its size and opens the editor; Markdown reads rendered
        d.click("tab:chat")
        settle(d)
        d.click("principal:grid")
        settle(d, 8)
        d.click("tab:fields")
        settle(d, 10)
        d.screenshot(os.path.join(shots, "fields.png"))
        d.click("button:edit:notes/long")
        pane = until("an editor on the Field", lambda: editor(d, "notes/long"))
        settle(d, 10)
        assert "button:editor-rendered" in d.targets(), sorted(t for t in d.targets() if "editor" in t)
        d.click("button:editor-rendered")
        settle(d, 8)
        assert editor(d, "notes/long")["rendered"] is True
        d.screenshot(os.path.join(shots, "editor-rendered.png"))
        d.click("button:editor-rendered")
        settle(d, 4)

        # a second EDIT brings the same tab back
        d.click("tab:chat")
        settle(d)
        d.click("button:edit:notes/long")
        settle(d, 8)
        assert len([p for p in d.panes()["live"] if p.get("field") == "notes/long"]) == 1

        # a short Field keeps its row
        d.click("tab:chat")
        settle(d)
        d.resize(1280, 1100)  # the short row sits below the fold at 800
        settle(d, 8)
        d.click("button:edit:notes/short")
        settle(d, 8)
        assert editor(d, "notes/short") is None, "a short Field stayed in its row"
        # CLEAR asks, naming what goes: Escape and CANCEL's Enter leave it, shift clears at once
        def fields_view():
            d.click("tab:chat")
            settle(d)
            d.click("tab:fields")
            settle(d, 8)

        fields_view()
        for n in ("notes/gone", "notes/quick"):
            r = other.req("set-field", {"object": grid, "field": n, "type": "string", "value": "going", "reader": "both"})
            assert r.get("ok"), r
        until("the rows", lambda: "button:clear:notes/gone" in d.targets() and "button:clear:notes/quick" in d.targets())
        d.click("button:clear:notes/gone")
        until("the confirm", lambda: "button:confirm-clear:notes/gone" in d.targets())
        settle(d, 4)
        d.screenshot(os.path.join(shots, "clear-confirm.png"))
        d.key("Escape")
        until("Escape closes it", lambda: "button:confirm-clear:notes/gone" not in d.targets())
        assert "going" in json.dumps(field(grid, "notes/gone")), "Escape cleared it"
        d.click("button:clear:notes/gone")
        until("the confirm again", lambda: "button:cancel-clear:notes/gone" in d.targets())
        settle(d, 4)
        d.key("Return")  # the keyboard is on CANCEL
        until("Enter on CANCEL closes it", lambda: "button:confirm-clear:notes/gone" not in d.targets())
        assert "going" in json.dumps(field(grid, "notes/gone")), "Enter cleared it"
        d.click("button:clear:notes/gone")
        until("once more", lambda: "button:confirm-clear:notes/gone" in d.targets())
        d.click("button:confirm-clear:notes/gone")
        until("cleared", lambda: "going" not in json.dumps(field(grid, "notes/gone")))
        d.call("click", target="button:clear:notes/quick", shift=True)
        until("shift cleared at once", lambda: "going" not in json.dumps(field(grid, "notes/quick")))
        settle(d, 4)
        assert "button:confirm-clear:notes/quick" not in d.targets(), "shift still asked"
        print("[fields] ok")
        d.call("quit")
    finally:
        person.close()
        other.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
