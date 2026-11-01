#!/usr/bin/env python3
"""Run a command against a throwaway world: its own port, its own data, a scripted model.

    privateworld.py --port 7820 [--keep DIR] -- cmd --url {url} --token-file {token} ...

{url}, {token} and {root} in the command become the world's url, its person.token and its
root.token. The same values are in ORBIS_WORLD_URL, ORBIS_WORLD_TOKEN and ORBIS_WORLD_DATA. The world
and the model stop with the command, by pid. A world cannot be started on the main port, and when
the toolchain is missing the exit is 77, which ctest reads as skipped.

Importable too: `with PrivateWorld(7820) as w:` gives w.url, w.token, w.root, w.data.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_spec = importlib.util.spec_from_file_location(
    "world_health", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts", "world-health.py"))
health = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(health)

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
MAIN_PORT = 7777
JDK = "/opt/homebrew/opt/openjdk@25"


class Model(BaseHTTPRequestHandler):
    """A chat completion stream driven by the last user turn, so a UX test sees real wakes.

    "Count slowly" streams for half a minute, so a test can stop or steer it. "run exactly this
    command: X." asks for task.run once, on the Principal's first answer to it. `ops {...}` in the
    text is sent back as an ops block, once. Anything else gets one short line."""

    answer = "I am root."
    ran = set()
    prompts = None

    def log_message(self, *a):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "scripted"}]}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _last_user(self, req):
        for m in reversed(req.get("messages") or []):
            if m.get("role") == "user":
                c = m.get("content")
                return c if isinstance(c, str) else json.dumps(c)
        return ""

    def _plan(self, text):
        m = re.search(r"run exactly this command: ([^\n]+?)\.\s*(?:\n|$)", text)
        cmd = m.group(1) if m else None
        if cmd and cmd not in Model.ran:
            Model.ran.add(cmd)
            op = {"op": "act", "args": {"action": "task.run", "args": {"adapter": "shell.local", "cmd": cmd}}}
            return ["```ops\n" + json.dumps(op) + "\n```"], 0.0
        m = re.search(r"^ops (\{.+\})\s*$", text, re.M)
        if m and m.group(1) not in Model.ran:
            Model.ran.add(m.group(1))
            return ["```ops\n" + m.group(1) + "\n```"], 0.0
        m = re.search(r"reply exactly: (.+)$", text, re.S)
        if m:
            return [m.group(1).strip().replace("<nl>", "\n")], 0.0
        if "Count slowly" in text:
            return [f"{i} " for i in range(1, 300)], 0.1
        return [self.answer], 0.0

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["content-length"])))
        if Model.prompts:
            with open(Model.prompts, "w") as f:  # what the model was told last, for a test to read
                json.dump(req["messages"], f)
        parts, pace = self._plan(self._last_user(req))
        if "What did I just attach" in self._last_user(req):
            # the name must have reached the prompt: the real question this step asks
            parts = ["You attached README.md." if "README.md" in json.dumps(req["messages"]) else "Nothing."]
        seen = json.dumps(req["messages"]).count('"image_url"') // 2  # the part's type, and its key
        if seen and parts == [self.answer]:
            parts = [f"I see {seen} image(s)."]
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        try:
            for p in parts:
                self.wfile.write(b"data: " + json.dumps({"choices": [{"delta": {"content": p}}]}).encode() + b"\n\n")
                self.wfile.flush()
                if pace:
                    time.sleep(pace)
            end = {"choices": [{"delta": {}, "finish_reason": "stop"}],
                   "usage": {"prompt_tokens": 12, "completion_tokens": 3}}
            self.wfile.write(b"data: " + json.dumps(end).encode() + b"\n\ndata: [DONE]\n\n")
        except OSError:
            pass  # a cancel closes the stream


class Client:
    """The JSON protocol over a raw socket, standard library only."""

    def __init__(self, port, token):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(30)
        self.s.sendall((f"GET /ws?format=json HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += health.read_exact(self.s, 1)
        self.me = self.req("attach", {"token": token, "client-kind": "test"})["result"]["principal"]

    def req(self, op, args):
        rid = str(uuid.uuid4())
        self.s.sendall(health.frame(json.dumps({"id": rid, "op": op, "args": args})))
        while True:
            code, payload = health.read_frame(self.s)
            if code == 1:
                m = json.loads(payload)
                if m.get("id") == rid:
                    return m

    def act(self, action, args, obj=None):
        body = {"action": action, "args": args}
        if obj:
            body["object"] = obj
        return self.req("act", body)

    def close(self):
        self.s.close()


class Skip(Exception):
    pass


def seed_grid(c, data):
    project = os.path.join(data, "grid-project")
    os.makedirs(project, exist_ok=True)
    with open(os.path.join(project, "README.md"), "w") as f:
        f.write("# Grid\n\nA project for tests.\n\n$$\\frac{a}{b}$$\n")
    r = c.act("principal.create", {"name": "grid", "charter": "A test project.",
                                   "grant": {"scope": {"prefix": "grid"}, "ops": ["read", "write", "reference", "transmit"]}})
    assert r.get("ok"), r
    r = c.act("workspace.mount", {"principal": "grid", "name": "grid", "path": project, "shell": True})
    assert r.get("ok"), r


class PrivateWorld:
    def __init__(self, port, keep=None, boot_seconds=180, seed=True, holder=False):
        self.holder_wanted = holder
        self.holder = None
        if port == MAIN_PORT:
            raise SystemExit("privateworld: 7777 is the main world's port")
        self.port = port
        self.keep = keep
        self.boot_seconds = boot_seconds
        self.seed = seed
        self.url = f"ws://127.0.0.1:{port}/ws"
        self.proc = None
        self.model = None

    def __enter__(self):
        clojure = shutil.which("clojure") or ("/opt/homebrew/bin/clojure" if os.path.exists("/opt/homebrew/bin/clojure") else None)
        if not clojure:
            raise Skip("no clojure")
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
            raise SystemExit(f"privateworld: something already answers on {self.port}")
        except OSError:
            pass
        self.data = self.keep or tempfile.mkdtemp(prefix="orbis-world-")
        os.makedirs(self.data, exist_ok=True)
        self.token = os.path.join(self.data, "person.token")
        self.root = os.path.join(self.data, "root.token")
        Model.prompts = os.path.join(self.data, "last-prompt.json")
        self.model = ThreadingHTTPServer(("127.0.0.1", self.port + 1), Model)
        threading.Thread(target=self.model.serve_forever, daemon=True).start()
        env = dict(os.environ, KE_PORT=str(self.port), KE_DATA=self.data, KE_FSYNC="never",
                   KE_MODEL_URL=f"http://127.0.0.1:{self.port + 1}/v1", KE_MODEL_KEY="x", KE_MODEL_ID="scripted",
                   KE_WORLD_NAME="private", KE_PUBLIC_URL=self.url,
                   KE_ROOT_LEASH=os.environ.get("KE_ROOT_LEASH", "0"))
        if self.holder_wanted:
            binary = os.path.join(REPO, "holder", "build", "holder")
            if not os.access(binary, os.X_OK):
                raise Skip("no holder binary: run make holder")
            sock = os.path.join(tempfile.mkdtemp(prefix="kh", dir="/tmp"), "s")
            self.holder = subprocess.Popen([binary, sock], env={"PATH": env["PATH"]})
            for _ in range(100):
                if os.path.exists(sock):
                    break
                time.sleep(0.02)
            env["KE_HOLDER_SOCK"] = sock
        if os.path.isdir(JDK):
            env["JAVA_HOME"] = JDK
            env["PATH"] = f"{JDK}/bin:/opt/homebrew/bin:" + env.get("PATH", "")
        self.log = open(os.path.join(self.data, "world.out"), "w")
        self.proc = subprocess.Popen([clojure, "-M:run"], cwd=os.path.join(REPO, "world"), env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        end = time.time() + self.boot_seconds
        while time.time() < end:
            if self.proc.poll() is not None:
                raise SystemExit(f"privateworld: world exited {self.proc.returncode}, see {self.data}/world.out")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                if os.path.exists(self.token) and os.path.getsize(self.token) > 0:
                    if self.seed:
                        self._seed()
                    return self
            except OSError:
                pass
            time.sleep(0.25)
        self.__exit__(None, None, None)
        raise SystemExit("privateworld: no world after waiting")

    def client(self, token_file=None):
        return Client(self.port, open(token_file or self.token).read().strip())

    def _seed(self):
        """A second Principal with its own mount, as a project would have: grid, grid.fs, grid.shell."""
        c = self.client(self.root)
        seed_grid(c, self.data)
        c.close()
    def __exit__(self, *exc):
        if self.holder:
            self.holder.terminate()
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
        if self.model:
            self.model.shutdown()
            self.model.server_close()
        if not self.keep and getattr(self, "data", None):
            shutil.rmtree(self.data, ignore_errors=True)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--keep", default=None)
    ap.add_argument("--no-seed", action="store_true", help="a world as a new person finds it")
    ap.add_argument("--holder", action="store_true", help="terminals in a real holder process")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    try:
        with PrivateWorld(a.port, a.keep, seed=not a.no_seed, holder=a.holder) as w:
            cmd = [c.replace("{url}", w.url).replace("{token}", w.token).replace("{root}", w.root) for c in cmd]
            env = dict(os.environ, ORBIS_TEST="1", ORBIS_WORLD_URL=w.url, ORBIS_WORLD_TOKEN=w.token, ORBIS_WORLD_DATA=w.data)
            return subprocess.call(cmd, env=env)
    except Skip as s:
        print("[privateworld] skipped:", s)
        return 77


if __name__ == "__main__":
    sys.exit(main())
