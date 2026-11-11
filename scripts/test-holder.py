#!/usr/bin/env python3
"""Terminals through restarts, for real: a supervisor, a holder, a world, a scripted model, a real box.
   scripts/test-holder.py <scratch-dir> [port]
1. a long program keeps running through world.restart and through a kill -9 of the world; every line
   reaches the record once, in order
2. input and the screen work afterwards
3. fast output: 30 MB through a confined terminal while the world still answers
4. from inside the box, the holder's socket and the tokens cannot be reached
5. a killed holder ends its terminals with what they showed; the next terminal has a new holder"""
import hashlib, importlib.util, json, os, signal, subprocess, sys, threading, time
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("sh", os.path.join(HERE, "test-selfhost.py"))
sh = importlib.util.module_from_spec(spec); spec.loader.exec_module(sh)
Client, wait, check = sh.Client, sh.wait, sh.check

def result(r):
    x = r.get("result") or {}
    return x.get("result", x)

def opened(c):
    r = c.act("pane.open", {"kind": "terminal", "target": "shell.local", "cols": 80, "rows": 24})
    return result(r)["pane"]

def screen(c, p):
    return result(c.act("pane.read", {"pane": p})).get("text") or ""

def state(c, p):
    return str(result(c.act("pane.read", {"pane": p})).get("state"))

def record(D, c, p):
    """The terminal's output as the record has it, read from the blobs its events name."""
    out = b""; after = 0; seen = set()
    while True:
        h = c.req("history", {"stream": p, "after": after, "limit": 500})["result"]
        for e in h.get("events") or []:
            n = e.get("event/seq")
            if n in seen or "pane-output" not in str(e.get("event/kind")):
                seen.add(n)
                continue
            seen.add(n)
            sha = e["event/body"]["blob"]
            out += open(f"{D}/blobs/sha256/{sha[:2]}/{sha}", "rb").read()
        if not h.get("next"):
            return out
        after = h["next"]

def rows_of(out):
    return [l for l in out.decode(errors="replace").replace("\r", "").split("\n") if l.startswith("row-") and l[4:].isdigit()]

def main(scratch, port):
    D = os.path.join(scratch, "w"); os.makedirs(D, exist_ok=True)
    model = ThreadingHTTPServer(("127.0.0.1", port + 1), sh.Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    subprocess.check_call([f"{HERE}/release.sh", "--data", D, "--repo", REPO, "--rev", "HEAD"], stdout=subprocess.DEVNULL)
    if not os.access(f"{D}/bin/holder", os.X_OK):
        print("no holder binary: run make holder"); return 77
    env = dict(os.environ, KE_PORT=str(port), KE_MODEL_URL=f"http://127.0.0.1:{port+1}/v1", KE_MODEL_KEY="x", KE_MODEL_ID="fake",
               KE_SETTLE_S="3", KE_HEALTH_S="90", KE_WORLD_NAME="holder-test")
    env.pop("KE_DATA", None)
    sup = subprocess.Popen([f"{HERE}/supervise.sh", D], env=env, stdout=subprocess.DEVNULL)
    try:
        tok = lambda: open(os.path.join(D, "person.token")).read().strip()
        c = wait("first start", lambda: Client(port, tok()))
        wait("good", lambda: os.path.exists(f"{D}/release.good"), 60)
        p = opened(c)
        wait("a prompt", lambda: screen(c, p).strip(), 20)

        # 1a. a restart in the middle of a program
        c.act("pane.send", {"pane": p, "text": "i=0; while [ $i -lt 12000 ]; do echo row-$i; i=$((i+1)); sleep 0.001; done; echo all-done\n"})
        wait("output", lambda: "row-" in screen(c, p), 20)
        r = c.act("world.restart", {"reason": "with a program running"})
        check("a person restarts the world under a running program", r.get("ok"))
        wait("the restart", lambda: "restart to" in open(f"{D}/logs/supervise.log").read(), 30)
        c = wait("the return", lambda: Client(port, tok()), 90)
        check("the terminal is still open", state(c, p) == "open")
        at = screen(c, p)
        check("it is still running: " + at.strip().splitlines()[-1], "all-done" not in at)

        # 1b. the world killed outright
        pid = int(open(f"{D}/world.pid").read())
        time.sleep(1.5)
        os.kill(pid, signal.SIGKILL)
        print("     killed the world", flush=True)
        time.sleep(2)
        c = wait("the return after kill -9", lambda: Client(port, tok()), 120)
        check("the terminal is open after kill -9", state(c, p) == "open")
        wait("the end of the program", lambda: "all-done" in screen(c, p), 120)

        # 2. it is still a terminal
        c.act("pane.send", {"pane": p, "text": "echo typed-after\n"})
        wait("input after", lambda: "\ntyped-after" in "\n" + screen(c, p), 15)
        check("input and the screen work after both", True)
        time.sleep(0.5)
        r = c.act("pane.close", {"pane": p}); time.sleep(0.5)
        rows = rows_of(record(D, c, p))
        check(f"every line once, in order, across both: {len(rows)}", rows == ["row-%d" % i for i in range(12000)])

        # 3. fast output, in the box
        p = opened(c)
        wait("a prompt", lambda: screen(c, p).strip(), 20)
        c.act("pane.send", {"pane": p, "text": "yes 0123456789012345678901234567890123456789 | head -c 30000000; echo done-marker\n"})
        t0 = time.time(); worst = 0.0
        while "done-marker" not in screen(c, p).replace("echo done-marker", ""):
            t1 = time.time()
            c.req("hello", {})
            worst = max(worst, time.time() - t1)
            if time.time() - t0 > 120:
                raise SystemExit("FAILED: fast output took too long")
            time.sleep(0.2)
        check(f"30 MB in {time.time()-t0:.1f}s, the world answering within {worst*1000:.0f} ms", worst < 2.0)

        # 4. the box still holds, and the holder's socket is inside what it forbids
        sock = env_sock(D, sup.pid)
        probe = ("clear; python3 -c \"import socket;s=socket.socket(socket.AF_UNIX);s.connect('%s');print('REACHED')\" 2>&1 | tail -1; "
                 "cat %s/person.token %s/root.token; echo probe-end\n" % (sock, D, D))
        c.act("pane.send", {"pane": p, "text": probe})
        wait("the probe", lambda: "\nprobe-end" in "\n" + screen(c, p), 20)
        text = screen(c, p)
        refused = "PermissionError" in text or "not permitted" in text
        check("the box stops the socket at connect: " + next((l for l in text.splitlines() if "Error" in l), "?")[:60],
              "\nREACHED" not in text and refused)
        check("and the tokens", open(f"{D}/person.token").read().strip() not in text)

        # 5. a killed holder
        c.act("pane.send", {"pane": p, "text": "clear; echo before-the-crash\n"})
        wait("the line", lambda: "\nbefore-the-crash" in "\n" + screen(c, p), 15)
        hp = int(open(f"{D}/holder.pid").read())
        os.kill(hp, signal.SIGKILL)
        wait("exited", lambda: state(c, p) == "exited", 20)
        check("a killed holder ends its terminals with what they showed", "before-the-crash" in screen(c, p))
        wait("a new holder", lambda: int(open(f"{D}/holder.pid").read()) != hp, 20)
        q = wait("a new terminal", lambda: opened(c), 20)
        wait("its prompt", lambda: screen(c, q).strip(), 20)
        c.act("pane.send", {"pane": q, "text": "echo fresh-holder\n"})
        wait("fresh", lambda: "\nfresh-holder" in "\n" + screen(c, q), 15)
        check("the next terminal runs in a new holder", True)
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(30)
        except subprocess.TimeoutExpired:
            sup.kill()
    check("a full stop ends the holder", not sh_alive(D))
    print("all good")
    return 0

def env_sock(D, sup_pid):
    p = f"{D}/holder.sock"
    if os.path.exists(p):
        return p
    # a long data dir: the supervisor chose a short path, and the holder has it open
    out = subprocess.check_output(["lsof", "-nP", "-a", "-U", "-p", open(f"{D}/holder.pid").read().strip()], text=True)
    return [w for l in out.splitlines() for w in l.split() if w.endswith("holder.sock")][0]

def sh_alive(D):
    return os.path.exists(f"{D}/holder.pid")

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 7842))
