#!/usr/bin/env python3
"""UX smoke through the drive API, against a live world.

    privateworld.py --port 7820 -- smoke_drive.py --listener <binary> --url {url} --token-file {root} --out <dir>

Launches the listener with --drive, waits for attach, screenshots, opens GRAPH, screenshots,
opens root's terminal, types a /fast question, waits for the reply (with its cost line), checks
that Escape clears a typed line, screenshots, quits. Exit 0 on pass, 1 on failure, 77 when no
world answers at the url (ctest reads 77 as skipped).
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


def world_reachable(url, timeout=2.0):
    u = urlparse(url)
    try:
        with socket.create_connection((u.hostname, u.port or 80), timeout=timeout):
            return True
    except OSError:
        return False


def step(name, fn):
    print(f"[ux] {name} ...", end=" ", flush=True)
    t = time.time()
    r = fn()
    print(f"ok ({time.time() - t:.1f}s)")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--out", default="ux-out")
    ap.add_argument("--reply-timeout", type=int, default=180000)
    ap.add_argument("--question", default="/fast In one short line: what is your name and how many routes do you have?")
    a = ap.parse_args()
    if urlparse(a.url).port == 7777:
        print("[ux] 7777 is the main world: this test types into its world, use privateworld.py")
        return 1

    if not world_reachable(a.url):
        print(f"[ux] no world at {a.url}: skipped")
        return 77
    if not os.path.exists(a.token_file):
        print(f"[ux] no token file {a.token_file}: skipped")
        return 77
    os.makedirs(a.out, exist_ok=True)
    # AF_UNIX paths are capped at 104 bytes on macOS: keep the socket in a short temp dir
    tmp = tempfile.mkdtemp(prefix="orbis-ux-", dir="/tmp")
    sock = os.path.join(tmp, "d.sock")
    # its own session.json: a listener already open on this Mac holds the default one, and the
    # world keeps one channel per session, so sharing it would make each attach close the other
    data_dir = os.path.join(tmp, "data")
    os.makedirs(data_dir, exist_ok=True)
    log = open(os.path.join(a.out, "listener.log"), "w")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", data_dir],
                            stdout=log, stderr=subprocess.STDOUT)
    failures = []
    d = None
    try:
        d = Drive(sock, connect_timeout=30.0)
        step("attached", lambda: d.wait("attached", timeout_ms=30000))
        step("route", lambda: d.wait("route", timeout_ms=30000))
        step("settle", lambda: d.wait("settle", frames=10, timeout_ms=5000))
        step("screenshot 01-attached", lambda: d.screenshot(os.path.join(a.out, "01-attached.png")))
        step("click tab:GRAPH", lambda: d.click("tab:graph"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        step("screenshot 02-graph", lambda: d.screenshot(os.path.join(a.out, "02-graph.png")))
        step("click tab:chat", lambda: d.click("tab:chat"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        st = d.state()
        name = st["principal_name"] or "root"
        step(f"click principal:{name}", lambda: d.click(f"principal:{name}"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        step("click input", lambda: d.click("input"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        replies_before = d.state()["replies"]
        step("type question", lambda: d.type(a.question))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        typed = d.state()["input_text"]
        if typed != a.question:
            failures.append(f"typed text differs: {typed!r}")
        step("key Return", lambda: d.key("Return"))
        step("wait reply", lambda: d.wait("reply", timeout_ms=a.reply_timeout))
        st = d.state()
        if st["replies"] <= replies_before:
            failures.append("no new reply counted")
        reply = [l for l in st["transcript"] if not l["mine"] and l["ready"] and l["text"]]
        if not reply:
            failures.append("no reply block in the transcript")
        else:
            print(f"[ux] reply: {reply[-1]['text'][:200]!r}")
        # the cost line under a reply comes from its `via` Evaluation's usage/context Fields,
        # which can land a few events after the Message: allow the fold a moment
        cost = {}
        for _ in range(20):
            lines = [l for l in d.state()["transcript"] if not l["mine"] and l["ready"] and l["text"]]
            cost = (lines[-1].get("cost") or {}) if lines else {}
            if cost.get("tokens_in", -1) >= 0 or cost.get("ctx_bytes", -1) >= 0:
                break
            d.wait("settle", frames=15, timeout_ms=10000)
        if cost.get("tokens_in", -1) < 0 and cost.get("ctx_bytes", -1) < 0:
            failures.append(f"reply has no cost line (via Evaluation without usage/context): {cost}")
        else:
            print(f"[ux] cost: {cost}")
        step("screenshot 03-reply", lambda: d.screenshot(os.path.join(a.out, "03-reply.png")))
        step("click input", lambda: d.click("input"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("type abc", lambda: d.type("abc"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        before = d.state()["input_text"]
        step("key Escape", lambda: d.key("Escape"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        after = d.state()["input_text"]
        if before != "abc" or after != "":
            failures.append(f"Escape did not clear the line: before={before!r} after={after!r}")
        step("screenshot 04-esc", lambda: d.screenshot(os.path.join(a.out, "04-esc.png")))

        # STOP and Esc-Esc: a /think question, then the running Evaluation ends cancelled. The first
        # version of the cancel frame had no args and the world refused every one.
        def run_and_stop(how):
            step("click input", lambda: d.click("input"))
            step("settle", lambda: d.wait("settle", frames=3, timeout_ms=5000))
            step("type /think question", lambda: d.type("/think Count slowly to five and explain each number."))
            step("key Return", lambda: d.key("Return"))
            for _ in range(60):
                if "button:stop" in d.state()["targets"]:
                    break
                time.sleep(0.25)
            time.sleep(1.5)
            since = d.state().get("head_seq", 0)
            if how == "stop":
                step("click button:stop", lambda: d.click("button:stop"))
            else:
                step("key Escape", lambda: d.key("Escape"))
                step("key Escape", lambda: d.key("Escape"))
            # The log, not the newest state: a queued line wakes the loop the moment a cancel lands.
            state = None
            for _ in range(60):
                st = d.state()
                state = (st.get("evaluation") or {}).get("state")
                if any(e["seq"] > since and e["kind"] == "evaluated" and e["body"].get("state") == "cancelled" for e in st.get("log") or []):
                    print(f"[ux] {how}: cancelled")
                    return
                time.sleep(0.25)
            failures.append(f"{how} did not cancel an Evaluation: state {state!r}")

        run_and_stop("stop")
        run_and_stop("esc-esc")

        # MEMORY: create the defaults when the Principal has none, then the four Fields exist
        step("click tab:MEMORY", lambda: d.click("tab:MEMORY"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        if "button:create" in d.state()["targets"]:
            step("click button:create", lambda: d.click("button:create"))
        memory = {}
        for _ in range(40):
            memory = {k: v for k, v in d.state()["fields"].items() if k.startswith("memory/")}
            if all(k in memory for k in ("memory/notes", "memory/policy", "memory/budget", "memory/retention-days")):
                break
            d.wait("settle", frames=6, timeout_ms=5000)
        if len(memory) < 4:
            failures.append(f"memory Fields missing after CREATE DEFAULTS: {sorted(memory)}")
        step("screenshot 05-memory", lambda: d.screenshot(os.path.join(a.out, "05-memory.png")))

        # NEW MEMORY: one step to add an entry, then FORGET clears it (the log keeps the history)
        entry = f"test-{int(time.time())}"
        if "input:new-memory:name" not in d.targets():
            step("open the memory form", lambda: d.click("button:new-memory"))
            step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click input:new-memory:name", lambda: d.click("input:new-memory:name"))
        step("settle", lambda: d.wait("settle", frames=3, timeout_ms=5000))
        step("type memory name", lambda: d.type(entry))
        step("click input:new-memory:value", lambda: d.click("input:new-memory:value"))
        step("settle", lambda: d.wait("settle", frames=3, timeout_ms=5000))
        step("type memory value", lambda: d.type("remembered by the ux smoke"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click button:set-memory", lambda: d.click("button:set-memory"))
        landed = None
        for _ in range(60):
            landed = d.state()["fields"].get("memory/" + entry)
            if landed:
                break
            d.wait("settle", frames=6, timeout_ms=5000)
        if not landed or landed.get("value") != "remembered by the ux smoke":
            failures.append(f"memory/{entry} did not land: {landed!r}")
        else:
            print(f"[ux] memory/{entry} landed")
            for _ in range(40):
                st = d.state()
                win = next((w for w in st["windows"] if w["name"] == "CHAT"), None)
                rect = st["targets"].get(f"button:forget:memory/{entry}")
                if rect and win and win["y"] + 240 < rect["y"] < win["y"] + win["h"] - 30:
                    break
                d.scroll(f"{int(win['x'] + win['w'] / 2)},{int(win['y'] + win['h'] - 40)}", dy=-3)
                d.wait("settle", frames=3, timeout_ms=3000)
            step("click forget", lambda: d.click(f"button:forget:memory/{entry}"))
            step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
            step("confirm forget", lambda: d.click(f"button:confirm-forget:memory/{entry}"))
            gone = None
            for _ in range(60):
                gone = d.state()["fields"].get("memory/" + entry)
                if gone and gone.get("value") == "":
                    break
                d.wait("settle", frames=6, timeout_ms=5000)
            if not gone or gone.get("value") != "":
                failures.append(f"memory/{entry} was not forgotten: {gone!r}")
            else:
                print(f"[ux] memory/{entry} forgotten")
        step("screenshot 05b-memory", lambda: d.screenshot(os.path.join(a.out, "05b-memory.png")))

        # a person has no loop Fields of its own: the rest of the walk is on root's terminal
        step("click principal:root", lambda: d.click("principal:root"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        # FIELDS: set a hint on loop/reasoning (or the first loop/* Field when the world has not set it)
        step("click tab:FIELDS", lambda: d.click("tab:FIELDS"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        st = d.state()
        fields = st["fields"]
        loops = sorted(k for k in fields if k.startswith("loop/") and not k.endswith("/hint") and not k.endswith("/comment"))
        target = "loop/reasoning" if "loop/reasoning" in fields else (loops[0] if loops else None)
        if target is None:
            failures.append("no loop/* Field to hint on")
        else:
            win = next((w for w in st["windows"] if w["name"] == "CHAT"), None)
            key = "hint:" + target

            def in_view(r):
                return win is not None and win["y"] + 280 < r["y"] + r["h"] / 2 < win["y"] + win["h"] - 10  # under NEW FIELD

            rect = None
            for _ in range(40):
                rect = d.state()["targets"].get(key)
                if rect and in_view(rect):
                    break
                up = rect is not None and rect["y"] < win["y"] + 280
                d.scroll(f"{int(win['x'] + win['w'] / 2)},{int(win['y'] + win['h'] - 40)}", dy=1 if up else -1)
                d.wait("settle", frames=3, timeout_ms=3000)
            if not (rect and in_view(rect)):
                failures.append(f"could not scroll {key} into view")
            else:
                hint_text = f"ux smoke hint {int(time.time())}: off unless asked with /think"
                step(f"click {key}", lambda: d.click(key))
                step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
                step("type hint", lambda: d.type(hint_text))
                step("key Return", lambda: d.key("Return"))
                landed = None
                for _ in range(60):
                    landed = d.state()["fields"].get(target, {}).get("hint")
                    if landed == hint_text:
                        break
                    d.wait("settle", frames=6, timeout_ms=5000)
                if landed != hint_text:
                    failures.append(f"hint on {target} did not land: {landed!r}")
                else:
                    print(f"[ux] hint on {target}: {landed!r}")
                step("screenshot 06-hint", lambda: d.screenshot(os.path.join(a.out, "06-hint.png")))
        # NEW PRINCIPAL: the dialog opens from PRINCIPALS, built from action/principal.create, and cancels
        step("click button:new-principal", lambda: d.click("button:new-principal"))
        step("settle", lambda: d.wait("settle", frames=8, timeout_ms=5000))
        st = d.state()
        narrow = (st.get("principal_name") or "") != "root"  # a person's Grants do not cover creating Principals or Objects
        if "button:create" not in st["targets"] or "input:create:name" not in st["targets"]:
            if narrow:
                print(f"[ux] NEW PRINCIPAL dialog not available as {st.get('principal_name')!r} (expected: narrow Grant)")
            else:
                failures.append(f"NEW PRINCIPAL dialog did not open: {sorted(k for k in st['targets'] if k.startswith(('button:', 'input:create')))}")
        step("screenshot 07-new-principal", lambda: d.screenshot(os.path.join(a.out, "07-new-principal.png")))
        step("click button:cancel", lambda: d.click("button:cancel"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        if "button:create" in d.state()["targets"]:
            failures.append("NEW PRINCIPAL dialog still open after CANCEL")
        # NEW FIELD on an object that is not a Principal: a number on a Model, in its inspector tab
        step("unfold root", lambda: d.click("tree-toggle:root"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click tree:root:root.model", lambda: d.click("tree:root:root.model"))
        step("settle", lambda: d.wait("settle", frames=8, timeout_ms=5000))
        field = f"ux/level-{int(time.time())}"
        step("open the Field form", lambda: d.click("button:new-field"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click input:new-field:name", lambda: d.click("input:new-field:name"))
        step("settle", lambda: d.wait("settle", frames=3, timeout_ms=5000))
        step("type field name", lambda: d.type(field))
        step("open type menu", lambda: d.click("menu:new-field:type"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        step("pick number", lambda: d.click("menu:new-field:type:number"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        step("click input:new-field:value", lambda: d.click("input:new-field:value"))
        step("settle", lambda: d.wait("settle", frames=3, timeout_ms=5000))
        step("type value", lambda: d.type("3"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click button:set-field", lambda: d.click("button:set-field"))
        landed = None
        for _ in range(60):
            landed = d.state()["fields"].get(field)
            if landed:
                break
            d.wait("settle", frames=6, timeout_ms=5000)
        if not landed:
            print(f"[ux] {field} did not land as {d.state().get('session_principal')}")
            if (st.get("principal_name") or "") == "root":
                failures.append(f"NEW FIELD {field} did not land")
        elif landed.get("type") != "number":
            failures.append(f"NEW FIELD {field} has type {landed.get('type')!r}, not number")
        else:
            print(f"[ux] NEW FIELD {field} on root.model: {landed.get('value')} ({landed.get('type')})")
        step("screenshot 11-new-field", lambda: d.screenshot(os.path.join(a.out, "11-new-field.png")))
        step("click principal:root", lambda: d.click("principal:root"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        # ACTIONS: the terminal's menu lists the world's actions, task.run among them
        step("click tab:chat", lambda: d.click("tab:chat"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        step("click button:actions", lambda: d.click("button:actions"))
        step("settle", lambda: d.wait("settle", frames=6, timeout_ms=5000))
        st = d.state()
        menu = sorted(k for k in st["targets"] if k.startswith("menu:"))
        if "menu:task.run" not in st["targets"]:
            failures.append(f"ACTIONS menu does not list task.run: {menu}")
        else:
            print(f"[ux] actions menu: {menu}")
        step("screenshot 08-actions", lambda: d.screenshot(os.path.join(a.out, "08-actions.png")))
        step("key Escape", lambda: d.key("Escape"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        # DROP (sprint 1 L8): the listener README lands as an Object tagged on a Message; root is
        # then asked what was attached and should name the file.
        readme = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "README.md")
        readme = os.path.abspath(readme)
        step("click tab:transcript", lambda: d.click("tab:transcript"))
        step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
        seq_before = max([l["seq"] for l in d.state()["transcript"]] or [0])
        step("drop README.md", lambda: d.drop(path=readme, target="input"))
        attached = None
        for _ in range(60):
            st = d.state()
            att = st.get("attach") or {}
            if att.get("done") and att.get("err"):
                if narrow and "unresolved" in str(att.get("stage")):
                    print(f"[ux] drop refused as {st.get('principal_name')!r}: {att.get('stage')} (expected: narrow Grant)")
                    attached = "skipped"
                else:
                    failures.append(f"drop failed: {att.get('stage')}")
                break
            lines = [l for l in st["transcript"] if l["seq"] > seq_before and l.get("tags") and l["text"].startswith("attached:")]
            if lines:
                attached = lines[-1]
                break
            d.wait("settle", frames=8, timeout_ms=5000)
        if attached == "skipped":
            pass
        elif attached is None:
            if not any(f.startswith("drop failed") for f in failures):
                failures.append(f"no tagged 'attached:' Message after the drop (attach={d.state().get('attach')})")
        else:
            print(f"[ux] attached: {attached['text']!r} tags={attached['tags']}")
        step("screenshot 09-drop", lambda: d.screenshot(os.path.join(a.out, "09-drop.png")))
        if attached is not None and attached != "skipped":
            step("click input", lambda: d.click("input"))
            step("settle", lambda: d.wait("settle", frames=4, timeout_ms=5000))
            replies_before = d.state()["replies"]
            step("type attach question", lambda: d.type("/fast What did I just attach? Name the file."))
            step("key Return", lambda: d.key("Return"))
            step("wait reply", lambda: d.wait("reply", timeout_ms=a.reply_timeout))
            st = d.state()
            reply = [l for l in st["transcript"] if not l["mine"] and l["ready"] and l["text"] and l["seq"] > attached["seq"]]
            if st["replies"] <= replies_before or not reply:
                failures.append("no reply to the attach question")
            elif "readme" not in reply[-1]["text"].lower():
                failures.append(f"the reply does not name README.md: {reply[-1]['text'][:200]!r}")
            else:
                print(f"[ux] attach reply: {reply[-1]['text'][:200]!r}")
            step("screenshot 10-attach-reply", lambda: d.screenshot(os.path.join(a.out, "10-attach-reply.png")))
        step("quit", lambda: d.quit())
    except (DriveError, ConnectionError, OSError, subprocess.CalledProcessError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        if proc.poll() is None and d is not None:
            try:
                d.quit()
            except Exception:
                pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("listener did not exit after quit")
        log.close()
        try:
            os.unlink(sock)
            os.rmdir(os.path.dirname(sock))
        except OSError:
            pass
    if failures:
        print("[ux] FAIL")
        for f in failures:
            print("  -", f)
        print(f"[ux] log: {os.path.join(a.out, 'listener.log')}")
        return 1
    print(f"[ux] PASS, screenshots in {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
