#!/usr/bin/env python3
"""Is the world on this port up? Attach and read the log head, over the same protocol every client uses.
   scripts/world-health.py <port> <token-file>     exit 0 and the head, or exit 1
Standard library only: the supervisor must run with nothing installed."""
import base64, json, os, socket, struct, sys, uuid

def frame(text):
    data = text.encode()
    mask = os.urandom(4)
    n = len(data)
    head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))

def read_exact(s, n):
    out = b""
    while len(out) < n:
        b = s.recv(n - len(out))
        if not b:
            raise EOFError("closed")
        out += b
    return out

def read_frame(s):
    b0, b1 = read_exact(s, 2)
    n = b1 & 0x7f
    if n == 126:
        n = struct.unpack(">H", read_exact(s, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(s, 8))[0]
    return b0 & 0x0f, read_exact(s, n)

def main(port, token_file):
    token = open(token_file).read().strip()
    s = socket.create_connection(("127.0.0.1", int(port)), timeout=5)
    s.settimeout(10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws?format=json HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += read_exact(s, 1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        raise RuntimeError("no upgrade")
    rid = str(uuid.uuid4())
    s.sendall(frame(json.dumps({"id": rid, "op": "attach", "args": {"token": token, "client-kind": "supervisor"}})))
    while True:
        op, payload = read_frame(s)
        if op != 1:
            continue
        m = json.loads(payload)
        if m.get("id") == rid:
            if not m.get("ok"):
                raise RuntimeError(f"attach refused: {m.get('err')}")
            print(m.get("seq"))
            return

if __name__ == "__main__":
    try:
        main(sys.argv[1], sys.argv[2])
    except Exception as e:
        print(e, file=sys.stderr)
        sys.exit(1)
