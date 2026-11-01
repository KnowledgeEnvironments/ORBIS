"""A person's session on a local world, by the same protocol as a client. Standard library only.
The token is read from a path and never printed."""
import base64, json, os, socket, struct, uuid

def _frame(text):
    data = text.encode(); mask = os.urandom(4); n = len(data)
    head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else (bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n)))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))

class World:
    def __init__(self, token_file=None, port=None, kind="test"):
        # a private world by environment; never the main one by accident once these are set
        token_file = token_file or os.environ.get("ORBIS_BENCH_TOKEN", "/path/to/worlds/sky/person.token")
        port = port or int(os.environ.get("ORBIS_BENCH_PORT", "7777"))
        token = open(token_file).read().strip()
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5); self.s.settimeout(1900)  # task.approve lasts as long as the command it runs
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET /ws?format=json HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head: head += self._n(1)
        a = self.call("attach", {"token": token, "client-kind": kind})
        if not a.get("ok"): raise RuntimeError(str(a.get("err")))
        self.me = a["result"].get("principal")
    def _n(self, n):
        out = b""
        while len(out) < n:
            b = self.s.recv(n - len(out))
            if not b: raise EOFError
            out += b
        return out
    def _read(self):
        h = self._n(2); n = h[1] & 0x7f
        if n == 126: n = struct.unpack(">H", self._n(2))[0]
        elif n == 127: n = struct.unpack(">Q", self._n(8))[0]
        return h[0] & 0x0f, self._n(n)
    def call(self, op, args):
        rid = str(uuid.uuid4()); self.s.sendall(_frame(json.dumps({"id": rid, "op": op, "args": args})))
        while True:
            o, p = self._read()
            if o == 9: self.s.sendall(bytes([0x8a, 0x80]) + os.urandom(4)); continue
            if o != 1: continue
            m = json.loads(p)
            if m.get("id") == rid: return m
    def act(self, action, args=None, **kw):
        return self.call("act", dict({"action": action, "args": args or {}}, **kw))
    def resolve(self, name):
        r = self.call("resolve", {"name": name, "op": "read"})
        return (r.get("result") or {}).get("object")
    def fields(self, obj):
        r = self.call("read", {"object": obj})
        f = (r.get("result") or {}).get("object/fields", {}) if r.get("ok") else {}
        return {k: v.get("field/value") for k, v in f.items()}
    def head(self):
        return self.call("history", {"last": 1}).get("seq")
    def events(self, after):
        ev = []; c = after
        while True:
            h = self.call("history", {"after": c, "limit": 500})
            es = (h.get("result") or {}).get("events", [])
            if not es: return ev
            ev += es; c = es[-1]["event/seq"]
            if len(es) < 500: return ev
