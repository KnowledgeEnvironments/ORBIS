#!/usr/bin/env python3
"""The holder on its own, over its socket: no world. tests/holder_test.py <holder binary>
Output and screen, a query answered, wide text, a reconnect losing nothing, flow control under fast
output, exit codes, a stale socket, and the socket's mode. Exit 0 when all hold."""
import os, socket, stat, struct, subprocess, sys, tempfile, time

OPEN, WRITE, RESIZE, KILL, ACK, ATTACH, FORGET, TEXT = 2, 3, 4, 5, 6, 7, 8, 9
HELLO = 1
EV_HELLO, EV_PANE, EV_END, EV_OPENED, EV_ERROR, EV_TEXT = 101, 102, 103, 104, 105, 106
EV_OUTPUT, EV_SCREEN, EV_EXIT = 110, 111, 112

def strs(items):
    out = struct.pack(">H", len(items))
    for s in items:
        b = s.encode()
        out += struct.pack(">H", len(b)) + b
    return out

class Conn:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.settimeout(0.05)
        self.buf = b""
        self.frames = []
    def send(self, op, pid=b"", body=b""):
        pid = pid.encode() if isinstance(pid, str) else pid
        n = 2 + len(pid) + len(body)
        self.s.sendall(struct.pack(">IBB", n, op, len(pid)) + pid + body)
    def pump(self):
        try:
            d = self.s.recv(1 << 20)
            if d == b"":
                raise ConnectionError
            self.buf += d
        except socket.timeout:
            pass
        while len(self.buf) >= 4:
            n = struct.unpack(">I", self.buf[:4])[0]
            if len(self.buf) < 4 + n:
                break
            f = self.buf[4:4 + n]
            self.buf = self.buf[4 + n:]
            idlen = f[1]
            self.frames.append((f[0], f[2:2 + idlen].decode(), f[2 + idlen:]))
    def until(self, pred, secs=5, what="condition"):
        end = time.time() + secs
        while time.time() < end:
            self.pump()
            r = pred(self)
            if r:
                return r
        raise AssertionError("timed out waiting for " + what)
    def take(self, kind):
        got = [f for f in self.frames if f[0] == kind]
        self.frames = [f for f in self.frames if f[0] != kind]
        return got

def screen(body, into):
    cols, rows, cx, cy, flags, shape, sb, tl = struct.unpack(">HHHHBBIH", body[:16])
    at = 16
    title = body[at:at + tl].decode(); at += tl
    nm = body[at]; at += 1
    modes = [struct.unpack(">H", body[at + 2 * i:at + 2 * i + 2])[0] for i in range(nm)]; at += 2 * nm
    nrows = struct.unpack(">H", body[at:at + 2])[0]; at += 2
    for _ in range(nrows):
        y, ncells = struct.unpack(">HH", body[at:at + 4]); at += 4
        line = []
        for _ in range(ncells):
            n = body[at]; at += 1
            t = body[at:at + n].decode(); at += n
            n = body[at]; at += 1
            a = body[at:at + n].decode(); at += n
            line.append((t, a))
        into[y] = line
    return dict(cols=cols, rows=rows, cx=cx, cy=cy, flags=flags, shape=shape, scrolled=sb, title=title, modes=modes)

def text(rows):
    return "\n".join("".join(t for t, _ in rows[y]).rstrip() for y in sorted(rows))

def opened(c, pid, argv, cols=80, rows=24):
    body = struct.pack(">HH", cols, rows) + strs(["/"]) + strs(argv) + strs(["PATH=/usr/bin:/bin", "TERM=xterm-256color"])
    c.send(OPEN, pid, body)
    c.until(lambda c: [f for f in c.frames if f[1] == pid and f[0] in (EV_OPENED, EV_ERROR)], what="open")
    assert not c.take(EV_ERROR), "open failed"
    c.take(EV_OPENED)
    c.send(ATTACH, pid, struct.pack(">Q", 0))

def main(binary):
    d = tempfile.mkdtemp(prefix="kh", dir="/tmp")
    path = os.path.join(d, "s")
    h = subprocess.Popen([binary, path])
    for _ in range(100):
        if os.path.exists(path):
            break
        time.sleep(0.01)
    try:
        assert stat.S_IMODE(os.stat(path).st_mode) == 0o600, "socket is 0600"
        print("ok   socket is 0600")
        second = subprocess.run([binary, path], capture_output=True, timeout=5)
        assert second.returncode != 0, "a second holder refuses a live socket"
        print("ok   a second holder refuses a live socket")

        c = Conn(path)
        c.send(HELLO)
        c.until(lambda c: [f for f in c.frames if f[0] == EV_END], what="hello")
        assert struct.unpack(">H", c.take(EV_HELLO)[0][2])[0] == 1
        c.take(EV_END)

        # output, colour, the screen
        opened(c, "p1", ["/bin/sh", "-c", "printf '\\033[1;31mred\\033[0m plain\\r\\n'; sleep 30"])
        rows = {}
        def saw(c):
            for _, _, b in c.take(EV_SCREEN):
                screen(b, rows)
            return rows and "red plain" in text(rows)
        c.until(saw, what="screen")
        assert rows[0][0] == ("r", "1;38;5;1"), rows[0][:2]
        print("ok   text and colour reach the screen")

        # a query is answered by the terminal, to the program
        opened(c, "p2", ["/bin/sh", "-c", "stty raw -echo; printf '\\033[6n'; dd bs=1 count=6 2>/dev/null | od -c | head -1; sleep 30"])
        rows2 = {}
        def answered(c):
            for f in [f for f in c.frames if f[0] == EV_SCREEN and f[1] == "p2"]:
                screen(f[2], rows2)
            c.frames = [f for f in c.frames if not (f[0] == EV_SCREEN and f[1] == "p2")]
            return rows2 and "033" in text(rows2)
        c.until(answered, what="cursor report")
        print("ok   a query gets its answer from the terminal")

        # wide text, alternate screen, modes, title
        opened(c, "p3", ["/bin/sh", "-c", "printf '\\033]2;my title\\007\\033[?1049h\\033[?1000h\\033[?1006h\\033[?2004h\\033[5 q'; printf '日本'; sleep 30"])
        rows3 = {}; meta = {}
        def wide(c):
            for f in [f for f in c.frames if f[0] == EV_SCREEN and f[1] == "p3"]:
                meta.update(screen(f[2], rows3))
            c.frames = [f for f in c.frames if not (f[0] == EV_SCREEN and f[1] == "p3")]
            return rows3 and "日" in text(rows3)
        c.until(wide, what="wide text")
        assert rows3[0][0][0] == "日" and rows3[0][1][0] == "" and rows3[0][2][0] == "本", rows3[0][:3]
        assert meta["flags"] & 2 and meta["title"] == "my title" and meta["shape"] == 5, meta
        assert {1000, 1006, 2004} <= set(meta["modes"]), meta
        print("ok   wide cells, alternate screen, title, cursor shape, mouse and paste modes")

        # the world goes away: a program keeps running, a new connection loses nothing
        opened(c, "p4", ["/bin/sh", "-c", "i=0; while [ $i -lt 2000 ]; do echo line-$i; i=$((i+1)); sleep 0.0005; done; sleep 30"])
        got = bytearray()
        def fill(c):
            for f in [f for f in c.frames if f[0] == EV_OUTPUT and f[1] == "p4"]:
                off = struct.unpack(">Q", f[2][:8])[0]
                assert off == len(got), (off, len(got))
                got.extend(f[2][8:])
            c.frames = [f for f in c.frames if not (f[0] == EV_OUTPUT and f[1] == "p4")]
            return len(got) > 200
        c.until(fill, what="first output")
        c.s.close()
        time.sleep(0.5)
        c = Conn(path)
        c.send(HELLO)
        c.until(lambda c: [f for f in c.frames if f[0] == EV_END], what="hello again")
        listed = {f[1]: f[2] for f in c.take(EV_PANE)}
        c.take(EV_HELLO); c.take(EV_END)
        assert set(listed) == {"p1", "p2", "p3", "p4"}, listed
        c.send(ATTACH, "p4", struct.pack(">Q", len(got)))
        def complete(c):
            fill(c)
            c.send(ACK, "p4", struct.pack(">Q", len(got)))
            return b"line-1999\r\n" in got
        c.until(complete, secs=20, what="the rest of the output")
        lines = got.decode().split("\r\n")
        assert lines[:2000] == ["line-%d" % i for i in range(2000)], "nothing lost or doubled"
        print("ok   a reconnect loses nothing: 2000 lines in order")

        # fast output: flow control holds the program, nothing is dropped
        opened(c, "p5", ["/bin/sh", "-c", "yes 0123456789012345678901234567890123456789 | head -c 40000000; echo; echo done-marker; sleep 30"])
        total = 0; n = 0; tail = b""
        start = time.time()
        def fast(c):
            nonlocal total, n, tail
            for f in [f for f in c.frames if f[0] == EV_OUTPUT and f[1] == "p5"]:
                off = struct.unpack(">Q", f[2][:8])[0]
                assert off == total, (off, total)
                total += len(f[2]) - 8
                tail = (tail + f[2][8:])[-64:]
            c.frames = [f for f in c.frames if not (f[0] == EV_OUTPUT and f[1] == "p5") and not f[0] == EV_SCREEN]
            c.send(ACK, "p5", struct.pack(">Q", total))
            return b"done-marker" in tail
        c.until(fast, secs=60, what="40 MB through flow control")
        print("ok   40 MB in order through flow control in %.1fs" % (time.time() - start))

        # scrollback and text
        c.send(TEXT, "p4")
        c.until(lambda c: [f for f in c.frames if f[0] == EV_TEXT], what="text")
        t = c.take(EV_TEXT)[0][2].decode()
        assert "line-0\n" in t and "line-1999" in t, t[:80]
        print("ok   scrollback reads back as text")

        # exit codes, kill
        opened(c, "p6", ["/bin/sh", "-c", "exit 7"])
        r = c.until(lambda c: [f for f in c.frames if f[0] == EV_EXIT and f[1] == "p6"], what="exit")
        assert struct.unpack(">I", r[0][2])[0] == 7
        c.send(ATTACH, "p1", struct.pack(">Q", 0))
        c.send(KILL, "p1")
        r = c.until(lambda c: [f for f in c.frames if f[0] == EV_EXIT and f[1] == "p1"], what="kill")
        print("ok   exit code, and a hangup ends a program")

        # resize reaches the program
        opened(c, "p7", ["/bin/sh", "-c", "sleep 0.3; stty size; sleep 30"], cols=80, rows=24)
        c.send(RESIZE, "p7", struct.pack(">HH", 100, 30))
        rows7 = {}
        def resized(c):
            for f in [f for f in c.frames if f[0] == EV_SCREEN and f[1] == "p7"]:
                screen(f[2], rows7)
            c.frames = [f for f in c.frames if not (f[0] == EV_SCREEN and f[1] == "p7")]
            return rows7 and "30 100" in text(rows7)
        c.until(resized, what="resize")
        print("ok   a resize reaches the program and the screen")

        h.terminate()
        h.wait(timeout=5)
        assert not os.path.exists(path), "the socket is removed"
        print("ok   stopping ends the terminals and removes the socket")
        print("all good")
    finally:
        if h.poll() is None:
            h.kill()

if __name__ == "__main__":
    main(sys.argv[1])
