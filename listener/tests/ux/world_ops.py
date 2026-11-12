#!/usr/bin/env python3
"""Every bracket that changes the world, through the drive API, asserting the event reached the log.

    world_ops.py --listener <binary> --url ws://127.0.0.1:7840/ws --token-file <person token> --out <dir>

Run it against a private world: it writes Fields, retires and restores an Adapter, and changes an
Model. REVOKE and PAIR issue and revoke device tokens, so they run only with --token-ops. A step
that needs the model is reported as skipped when no model answers. Exit 0 on pass, 1 on failure, 77
when no world answers at the url (ctest reads 77 as skipped).
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive, DriveError  # noqa: E402


class Ops:
    def __init__(self, d, out="."):
        self.d = d
        self.out = out
        self.failures = []
        self.skipped = []

    def head(self):
        return self.d.state().get("head_seq", 0)

    def settle(self):
        self.d.wait("settle", frames=6, timeout_ms=30000)

    def right(self, target):
        self.d.call("click", target=target, button="right")
        self.settle()

    def target(self, name, seconds=30.0):
        end = time.time() + seconds
        while time.time() < end:
            if name in self.d.state()["targets"]:
                return True
            time.sleep(0.1)
        return False

    def event(self, since, kind, match=lambda e: True, seconds=60.0):
        end = time.time() + seconds
        while time.time() < end:
            for e in self.d.state().get("log") or []:
                if e["seq"] > since and e["kind"] == kind and match(e):
                    return e
            time.sleep(0.2)
        return None

    def check(self, name, ok, detail=""):
        print(f"[ops] {name}: {'ok' if ok else 'FAILED ' + detail}", flush=True)
        if not ok:
            self.failures.append(name)

    def skip(self, name, why):
        print(f"[ops] {name}: skipped, {why}", flush=True)
        self.skipped.append(name)

    def click(self, name):
        if not self.target(name):
            return False
        self.settle()  # a dialog places itself a frame after it first appears
        if not self.target(name, 1.0):
            return False  # a STOP can finish in that frame
        self.d.click(name)
        self.settle()
        return True

    def type_into(self, target, text):
        self.d.click(target)
        self.settle()
        self.d.type(text)
        self.settle()


def world_reachable(url):
    u = urlparse(url)
    try:
        with socket.create_connection((u.hostname, u.port or 80), timeout=2.0):
            return True
    except OSError:
        return False


def set_and_clear(o, agent):
    o.click(f"principal:{agent}")
    o.click("tab:fields")
    name = f"notes/ops-{int(time.time())}"
    o.click("button:new-field")
    o.type_into("input:new-field:name", name)
    o.type_into("input:new-field:value", "written by the world-op test")
    since = o.head()
    clicked = o.click("button:set-field")
    e = o.event(since, "field-set", lambda e: e["body"].get("name") == name)
    o.check("SET", clicked and e is not None, "no field-set")
    since = o.head()
    for _ in range(30):  # the new Field is usually below the fold of the table
        if f"button:clear:{name}" in o.d.state()["targets"]:
            break
        o.d.call("scroll", x=900, y=300, dy=-3.0)
        o.settle()
    clicked = o.click(f"button:clear:{name}") and o.click(f"button:confirm-clear:{name}")
    e = o.event(since, "field-set", lambda e: e["body"].get("name") == name and e["body"].get("cleared"))
    o.check("CLEAR", clicked and e is not None, "no cleared field-set")
    o.settle()
    o.check("CLEAR removes the Field", name not in (o.d.state().get("fields") or {}), "still in the table")


def delete_and_restore(o, name, owner="grid"):
    o.click(f"tree-toggle:{owner}")  # the tree starts folded
    o.right(f"tree:{owner}:{name}")
    if not o.click("menu:delete"):
        o.check("DELETE", False, "no menu item")
        return
    since = o.head()
    clicked = o.click("button:confirm-delete")
    if o.target("button:force-delete", 3.0):
        print("[ops] DELETE: dependents listed, forcing", flush=True)
        clicked = o.click("button:force-delete")
    e = o.event(since, "retired")
    o.check("DELETE", clicked and e is not None, "no retired event")
    o.check("DELETE leaves the tree", not o.target(f"tree:{owner}:{name}", 2.0), "still listed")
    since = o.head()
    o.click("button:show-deleted")
    o.target(f"button:restore:{name}", 4.0)
    targets = o.d.state()["targets"]
    # a fresh attach knows a deleted Object by id only: the world unbound its names
    row = f"button:restore:{name}" if f"button:restore:{name}" in targets else next((t for t in targets if t.startswith("button:restore:")), "")
    clicked = bool(row) and o.click(row)
    e = o.event(since, "restored")
    o.check("RESTORE", clicked and e is not None, "no restored event")
    if o.target("button:show-deleted", 2.0):
        o.click("button:show-deleted")
    o.check("RESTORE returns to the tree", o.target(f"tree:{owner}:{name}", 6.0), "not listed")
    o.click(f"tree-toggle:{owner}")


def refused_delete(o):
    since = o.head()
    o.right("principal:root")
    o.d.screenshot(os.path.join(o.out, "root-menu.png"))
    o.check("root has no DELETE", "menu:delete" not in o.d.state()["targets"], "the item is enabled")
    o.d.key("Escape")
    o.settle()
    o.check("root stays", o.event(since, "retired", seconds=1.0) is None, "retired")


def open_terminal(o, agent):
    end = time.time() + 6
    while time.time() < end:
        o.click(f"principal:{agent}")
        if o.d.state().get("principal_name") == agent:
            o.click("tab:transcript")
            return True
    o.check(f"open {agent}", False, "the tab did not open")
    return False


def model(o, agent, name):
    open_terminal(o, agent)
    since = o.head()
    if not o.click("button:model"):
        o.check("MODEL", False, "no button")
        return
    # a row is a Model and one of its offers: the first offer of this Model will do
    time.sleep(0.3)
    row = next((t for t in o.d.targets() if t.startswith(f"menu:model:{name}:")), None)
    clicked = row is not None and o.click(row)
    e = o.event(since, "field-set", lambda e: e["body"].get("name") == "principal/model-choice", seconds=6.0)
    if e is None:
        o.d.key("Escape")
    o.check("MODEL", clicked and e is not None, "no field-set of principal/model-choice")


def stop(o, agent):
    open_terminal(o, agent)
    o.type_into("input", "/think Count slowly to twenty and explain each number at length.")
    o.d.key("Return")
    if not o.target("button:stop", 30.0):
        o.skip("STOP", "no Evaluation started, the model may be down")
        return
    time.sleep(1.0)
    since = o.head()
    o.click("button:stop")
    e = o.event(since, "evaluated", lambda e: e["body"].get("state") == "cancelled" or e["body"].get("cancelled"), seconds=20.0)
    state = (o.d.state().get("evaluation") or {}).get("state")
    o.check("STOP", e is not None or state == "cancelled", f"evaluation state {state!r}")


def steer(o, agent):
    """Return queues while the agent works, the strip shows it, x takes it back, Shift-Return steers."""
    open_terminal(o, agent)
    o.type_into("input", "/think Count slowly to forty and explain each number at length.")
    o.d.key("Return")
    if not o.target("button:stop", 30.0):
        o.skip("QUEUE and STEER", "no Evaluation started")
        return
    time.sleep(1.0)
    o.type_into("input", "first queued line from the world-op test")
    o.d.key("Return")
    o.type_into("input", "second queued line from the world-op test")
    o.d.key("Return")
    end = time.time() + 10
    rows = []
    while time.time() < end and len(rows) < 2:
        rows = [r for r in o.d.state().get("queue") or [] if r.get("message")]
        time.sleep(0.2)
    o.check("RETURN queues while it works", len(rows) >= 2, f"queue {o.d.state().get('queue')}")
    if rows:
        o.d.screenshot(os.path.join(o.out, "queue.png"))
        gone = rows[-1]["message"]
        o.check("the queue strip can take a line back", o.click(f"button:dequeue:{gone[:8]}"), "no x")
        end = time.time() + 8
        while time.time() < end and any(r.get("message") == gone for r in o.d.state().get("queue") or []):
            time.sleep(0.2)
        o.check("loop.dequeue took it", all(r.get("message") != gone for r in o.d.state().get("queue") or []), "still queued")
    since = o.head()
    o.type_into("input", "actually stop counting and just say done")
    o.d.key("Return", mods=["shift"])
    e = o.event(since, "evaluated", lambda e: e["body"].get("state") == "cancelled" and e["body"].get("reason") == "steer", seconds=25.0)
    o.check("SHIFT-RETURN steers", e is not None, "no cancelled evaluation with reason steer")
    end = time.monotonic() + 30.0
    while time.monotonic() < end:
        state = o.d.state()
        # The map's actor state tracks the world's running Evaluation, not a
        # transient STOP target from the cancelled generation's UI frame.
        asker = next(a for a in state["map"]["actors"] if a["name"] == agent)
        if not asker["running"]:
            break
        if "button:stop" in state["targets"]:
            try:
                o.d.click("button:stop")
            except DriveError as exc:
                if "unknown target button:stop" not in str(exc):
                    raise
                # A steered Message can replace the Evaluation between frames.
                # Retry even when the next generation already has STOP again.
        time.sleep(0.1)
    else:
        raise AssertionError(f"{agent} still has a running Evaluation after 30 s")


def ask_for_run(o, agent, tag):
    """Asks the agent to run a gated command and returns the short id of the pending Task, or None."""
    open_terminal(o, agent)
    known = {t.get("id") for t in o.d.state().get("tasks") or []}
    o.type_into("input", f"/fast Using your shell adapter, run exactly this command: echo world-ops-{tag}-{int(time.time())}. This is a new command, so run it even if you ran one like it. One op, no prose.")
    o.d.key("Return")
    end = time.time() + 120
    while time.time() < end:
        for t in o.d.state().get("tasks") or []:
            if t.get("id") not in known:
                return t["id"]
        time.sleep(0.5)
    return None


def inbox(o, task):
    o.check("a card offers DENY as well as APPROVE", o.target(f"button:deny:{task[:8]}", 6.0) or f"button:approve:{task[:8]}" not in o.d.state()["targets"], "APPROVE alone")
    if not o.target("pane:approvals", 0.5):
        o.click("button:approvals")
    o.click(f"approval:{task[:8]}")


def approvals(o, agent):
    for word, button in (("APPROVE", "button:approve"), ("DENY", "button:deny")):
        task = ask_for_run(o, agent, word.lower())
        if not task:
            o.skip(word, "nothing asked; the model did not call the gate")
            continue
        inbox(o, task)
        since = o.head()
        clicked = o.click(button)
        o.check(word, clicked and o.event(since, "task-moved", seconds=15.0) is not None, "no task-moved")
    task = ask_for_run(o, agent, "always")
    if not task:
        o.skip("APPROVE AND REMEMBER", "nothing asked")
        return
    inbox(o, task)
    o.click("button:always")
    o.check("the rule is tested live", o.target("input:rule:programs", 5.0), "no form")
    clicked = o.click("button:approve-remember")
    o.click("button:approvals")
    # What a person sees: the rule in POLICY. It goes again, or the next run would never reach the gate.
    open_terminal(o, agent)
    o.click("tab:rules")
    end = time.time() + 15
    rows = []
    while time.time() < end and not rows:
        rows = [t for t in o.d.state()["targets"] if t.startswith("button:rule-remove:")]
        time.sleep(0.3)
    o.check("APPROVE AND REMEMBER", clicked and bool(rows), "no rule in POLICY")
    for row in rows:
        o.click(row)
    o.click("tab:transcript")


def policy(o, agent):
    open_terminal(o, agent)
    o.click("tab:rules")
    since = o.head()
    o.click("button:rule-new")
    o.type_into("input:rule:programs", "pwd")
    o.type_into("input:rule:note", "from the world-op test")
    clicked = o.click("button:rule-add")
    e = o.event(since, "field-set", lambda e: e["body"].get("name") == "policy/approval-rules", seconds=10.0)
    o.check("NEW RULE", clicked and e is not None, "no rule written")
    o.settle()
    rows = [t for t in o.d.state()["targets"] if t.startswith("button:rule-remove:")]
    for row in rows:
        since = o.head()
        o.click(row)
        e = o.event(since, "field-set", lambda e: e["body"].get("name") == "policy/approval-rules", seconds=10.0)
        o.check("REMOVE RULE", e is not None, "no rule removed")
    o.click("tab:transcript")


def conversations(o, agent):
    open_terminal(o, agent)
    o.click("tab:conversations")
    o.settle()
    end = time.time() + 20.0
    rows = []
    while time.time() < end and not rows:
        rows = [t for t in o.d.state()["targets"] if t.startswith("button:conversation-open:")]
        time.sleep(0.1)
    if not rows:
        o.skip("CONVERSATIONS", "this world does not list them yet")
    else:
        o.check("CONVERSATIONS lists threads", True)
    o.click("tab:transcript")


def off_the_record(o, agent):
    since = o.head()
    o.right(f"principal:{agent}")
    if not o.click("menu:off-the-record"):
        o.check("OFF THE RECORD", False, "no menu item")
        return
    o.check("OFF THE RECORD asks first", o.d.state().get("scratch") == 0, "began without confirming")
    clicked = o.click("button:scratch-begin")
    end = time.time() + 10
    while time.time() < end and o.d.state().get("scratch") != 1:
        time.sleep(0.2)
    o.check("OFF THE RECORD begins", clicked and o.d.state().get("scratch") == 1, "no session")
    o.check("OFF THE RECORD is on the log", o.event(since, "scratch-began") is not None, "no scratch-began")
    try:
        o.d.type("this must not be driven")
        o.check("OFF THE RECORD refuses the drive", False, "type was accepted")
    except Exception as e:  # DriveError carries the err text
        o.check("OFF THE RECORD refuses the drive", "ui-only" in str(e), str(e))
    state = o.d.state()
    o.check("OFF THE RECORD keeps no text in drive state", "must not" not in str(state.get("transcript")), "text in state")
    o.click("button:scratch-end")
    o.click("button:scratch-end-confirm")
    end = time.time() + 6
    while time.time() < end and o.d.state().get("scratch") != 0:
        time.sleep(0.2)
    o.check("OFF THE RECORD ends", o.d.state().get("scratch") == 0, "still open")
    o.check("OFF THE RECORD end is on the log", o.event(since, "scratch-ended") is not None, "no scratch-ended")


def token_ops(o, agent):
    o.click(f"principal:{agent}")
    o.click("tab:devices")
    since = o.head()
    opened = o.click("button:pair-phone")
    clicked = opened and o.click("button:pair-begin")
    o.check("PAIR", clicked and o.event(since, "created", seconds=8.0) is not None, "no pairing created")
    revoke = next((t for t in o.d.state()["targets"] if t.startswith("button:revoke:")), None)
    if not revoke:
        o.skip("REVOKE", "no live device")
        return
    since = o.head()
    o.click(revoke)
    # a token's revocation is a Field on its Object; `revoked` is the event of a Grant
    o.check("REVOKE", o.event(since, "field-set", lambda e: "token/revoked-at" in str(e.get("body"))) is not None,
            "no token/revoked-at")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--out", default="ops-out")
    ap.add_argument("--agent", default="grid")
    ap.add_argument("--adapter", default="grid.shell")
    ap.add_argument("--asker", default="root", help="the agent asked to run a gated command")
    ap.add_argument("--model", default="root.model")
    ap.add_argument("--token-ops", action="store_true")
    a = ap.parse_args()
    if not world_reachable(a.url) or not os.path.exists(a.token_file):
        print("[ops] no world or no token: skipped")
        return 77
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="orbis-ops-", dir="/tmp")
    sock = os.path.join(tmp, "d.sock")
    data = os.path.join(tmp, "data")
    os.makedirs(data)
    log = open(os.path.join(a.out, "listener.log"), "w")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock, "--data-dir", data],
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        d = Drive(sock)
        d.wait("attached", timeout_ms=20000)
        d.wait("route", timeout_ms=20000)
        end = time.time() + 60.0  # the fold boots: the object list has its rows
        while time.time() < end and not any(t.startswith("principal:") for t in d.state()["targets"]):
            time.sleep(0.1)
        o = Ops(d, a.out)
        set_and_clear(o, a.agent)
        delete_and_restore(o, a.adapter)
        refused_delete(o)
        model(o, a.agent, a.model)
        stop(o, a.agent)
        steer(o, a.asker)
        approvals(o, a.asker)
        policy(o, a.agent)
        conversations(o, a.agent)
        off_the_record(o, a.agent)
        if a.token_ops:
            token_ops(o, a.agent)
        else:
            o.skip("PAIR and REVOKE", "they change tokens; pass --token-ops on a throwaway world")
        d.screenshot(os.path.join(a.out, "ops.png"))
        d.quit()
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    print(f"[ops] {'PASS' if not o.failures else 'FAIL ' + ', '.join(o.failures)}; skipped: {', '.join(o.skipped) or 'none'}")
    return 1 if o.failures else 0


if __name__ == "__main__":
    sys.exit(main())
