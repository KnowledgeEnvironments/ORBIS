#!/usr/bin/env python3
"""The whole self-hosting loop, for real, on a private port: a supervisor, a world, a scripted model.
   scripts/test-selfhost.py <scratch-dir> [port]
1. a loop asks for a restart in the middle of a wake; the world comes back and the wake answers
2. a person moves the world to a new good release; the mounts follow
3. a release that cannot start comes back to the last good one by itself, and the person is told
Uses the JSON protocol through a standard-library client, so it needs nothing installed."""
import importlib.util, json, os, shutil, signal, socket, struct, subprocess, sys, threading, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("wh", os.path.join(HERE, "world-health.py"))
wh = importlib.util.module_from_spec(spec); spec.loader.exec_module(wh)

ASK = "PLEASE-RESTART"
DONE = "answered after the restart"
calls = {"asked": 0}

class Model(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = json.dumps({"data": [{"id": "fake"}]}).encode()
        self.send_response(200); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["content-length"])))
        text = "ok"
        if ASK in json.dumps(req["messages"]):
            calls["asked"] += 1
            text = ('```ops\n{"op":"act","args":{"action":"world.restart","args":{"reason":"mid-wake"}}}\n```'
                    if calls["asked"] == 1 else DONE)
        chunks = [{"choices": [{"delta": {"content": text}}]},
                  {"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 1, "completion_tokens": 1}}]
        self.send_response(200); self.send_header("content-type", "text/event-stream"); self.end_headers()
        for c in chunks:
            self.wfile.write(b"data: " + json.dumps(c).encode() + b"\n\n")
        self.wfile.write(b"data: [DONE]\n\n")

class Client:
    def __init__(self, port, token):
        s = socket.create_connection(("127.0.0.1", port), timeout=5); s.settimeout(30); self.s = s
        s.sendall((f"GET /ws?format=json HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        h = b""
        while b"\r\n\r\n" not in h:
            h += wh.read_exact(s, 1)
        self.me = self.req("attach", {"token": token, "client-kind": "test"})["result"]["principal"]
    def req(self, op, args):
        rid = str(uuid.uuid4())
        self.s.sendall(wh.frame(json.dumps({"id": rid, "op": op, "args": args})) if len(json.dumps(args)) < 60000 else b"")
        while True:
            code, payload = wh.read_frame(self.s)
            if code != 1:
                continue
            m = json.loads(payload)
            if m.get("id") == rid:
                return m
    def act(self, action, args):
        return self.req("act", {"action": action, "args": args})

def wait(what, pred, seconds=240):
    end = time.time() + seconds
    while time.time() < end:
        try:
            v = pred()
            if v:
                return v
        except (OSError, EOFError, ValueError):
            pass
        time.sleep(1)
    raise SystemExit(f"FAILED waiting for {what}")

def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        raise SystemExit(1)

def main(scratch, port):
    D = os.path.join(scratch, "world"); os.makedirs(D, exist_ok=True)
    model = ThreadingHTTPServer(("127.0.0.1", port + 1), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    first = subprocess.check_output([f"{HERE}/release.sh", "--data", D, "--repo", REPO, "--rev", "HEAD"], text=True).strip()
    env = dict(os.environ, KE_PORT=str(port), KE_MODEL_URL=f"http://127.0.0.1:{port+1}/v1", KE_MODEL_KEY="x", KE_MODEL_ID="fake",
               KE_SETTLE_S="5", KE_HEALTH_S="90", KE_WORLD_NAME="selfhost-test")
    env.pop("KE_DATA", None)
    sup = subprocess.Popen([f"{HERE}/supervise.sh", D], env=env, stdout=subprocess.DEVNULL)
    try:
        tok = lambda: open(os.path.join(D, "person.token")).read().strip()
        c = wait("first start", lambda: Client(port, tok()))
        world = wait("world/release", lambda: c.req("hello", {}).get("result", {}).get("world") or True)
        check("a supervised world runs from a release", os.path.isfile(f"{D}/releases/{first}/release.edn"))
        try:
            open(f"{D}/releases/{first}/README.md", "a").close()
            check("the running release is read-only", False)
        except PermissionError:
            check("the running release is read-only", True)
        wait("first good", lambda: os.path.exists(f"{D}/release.good"), 60)

        # 1. a loop restarts the world from inside a wake and carries on
        root = c.req("resolve", {"name": "root", "op": "read"})["result"]["object"]
        route = c.req("resolve", {"name": "root.route", "op": "read"})["result"]["object"]
        r = c.act("approval.unsafe", {"principal": root, "minutes": 10})
        check("a person arms unsafe mode on root", r.get("ok"))
        r = c.req("send", {"to": [root], "route": route, "text": ASK})
        check("a person asks root", r.get("ok")); ask = r["result"]["message"]
        print("     waiting for the world to go and come back", flush=True)
        wait("the restart", lambda: "restart to" in open(f"{D}/logs/supervise.log").read())
        c = wait("the return", lambda: Client(port, tok()))
        def reply():
            h = c.req("history", {"stream": c.me, "limit": 200})
            for e in reversed(h["result"]["events"]):
                if e.get("event/kind") in ("delivered", "delivered".encode()) or "delivered" in str(e.get("event/kind")):
                    m = e["event/body"].get("message")
                    f = c.req("read", {"object": m}).get("result", {}).get("object/fields", {})
                    t = (f.get("message/text") or {}).get("field/value", "")
                    if DONE in str(t):
                        return t
        check("the wake resumed and answered: " + str(wait("the resumed answer", reply, 120))[:40], True)
        check("the model was asked exactly twice", calls["asked"] == 2)
        wait("settled", lambda: "good" in open(f"{D}/logs/supervise.log").read().split("restart to")[-1], 60)

        # 2. a good new release, from the working copy
        with open(f"{D}/src/NEW-RELEASE.md", "w") as f:
            f.write("made in the working copy\n")
        subprocess.check_call(["git", "-C", f"{D}/src", "add", "-A"])
        subprocess.check_call(["git", "-C", f"{D}/src", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "work"])
        good = subprocess.check_output([f"{HERE}/release.sh", "--data", D, "--repo", f"{D}/src", "--rev", "HEAD"], text=True).strip()
        r = c.act("world.restart", {"release": good, "reason": "a new release"})
        check("a person moves to a new release", r.get("ok") and True)
        wait("the new release", lambda: open(f"{D}/release").read().strip() == good and "healthy "+good[:12] in open(f"{D}/logs/supervise.log").read())
        c = wait("the return", lambda: Client(port, tok()))
        wait("good", lambda: open(f"{D}/release.good").read().strip() == good, 60)
        names = c.req("read", {"name": "harness/NEW-RELEASE.md"})
        check("the harness mount follows the release", names.get("ok"))

        # 3. a release that cannot start
        p = f"{D}/src/world/src/knowledge/world/main.clj"
        s = open(p).read().replace("(defn -main [& _]", "(defn -main [& _]\n  (System/exit 3)")
        open(p, "w").write(s)
        subprocess.check_call(["git", "-C", f"{D}/src", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qam", "broken"])
        bad = subprocess.check_output([f"{HERE}/release.sh", "--data", D, "--repo", f"{D}/src", "--rev", "HEAD"], text=True).strip()
        r = c.act("world.restart", {"release": bad, "reason": "this one is broken"})
        check("a person moves to a broken release", r.get("ok"))
        wait("the rollback", lambda: "back to" in open(f"{D}/logs/supervise.log").read())
        check("the data dir names the good release again", open(f"{D}/release").read().strip() == good)
        c = wait("the return", lambda: Client(port, tok()))
        def told():
            rows = c.act("notify.list", {"limit": 5})["result"]["result"]["rows"]
            return [x for x in rows if x["level"] == "urgent" and "failed" in x["text"]]
        rows = wait("the notification", told, 60)
        check("the person is told: " + rows[0]["text"], True)
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(30)
        except subprocess.TimeoutExpired:
            sup.kill()
    print("all good")

if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 7801)
