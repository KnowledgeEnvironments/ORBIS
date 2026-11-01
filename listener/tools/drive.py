#!/usr/bin/env python3
"""Client for the listener's drive API (listener --drive <socket>).

Python API:
    from drive import Drive
    d = Drive("/tmp/orbis.sock")
    d.wait("attached", timeout_ms=20000)
    d.click("tab:GRAPH"); d.screenshot("graph.png")
    d.click("principal:root"); d.click("input"); d.type("/fast hello"); d.key("Return")
    d.wait("reply", timeout_ms=180000); print(d.state()["transcript"][-1]["text"])

CLI (commands run in order, each waits for its answer):
    drive.py --socket /tmp/orbis.sock screenshot out.png
    drive.py --socket /tmp/orbis.sock click tab:GRAPH wait settle screenshot graph.png
    drive.py --socket /tmp/orbis.sock click input type "hello" key Return wait reply --timeout 120000 state
    drive.py --socket /tmp/orbis.sock key w --mods cmd
    drive.py --socket /tmp/orbis.sock targets
    drive.py --socket /tmp/orbis.sock drop /path/to/file.md wait text --text "attached:" state

Screenshots: the listener writes a BMP; a .png path is converted here with sips (macOS).
No third-party dependencies.
"""
import json
import os
import socket
import subprocess
import sys
import time


class DriveError(RuntimeError):
    def __init__(self, reply):
        super().__init__(reply.get("err", "drive error"))
        self.reply = reply


class Drive:
    def __init__(self, socket_path, connect_timeout=20.0):
        self.path = socket_path
        self._id = 0
        self._buf = b""
        deadline = time.time() + connect_timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(socket_path)
                break
            except OSError:
                self.sock.close()
                if time.time() >= deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(30.0)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # --- transport ---------------------------------------------------------------------------
    def call(self, cmd, timeout=None, **args):
        self._id += 1
        req = {"id": self._id, "cmd": cmd}
        req.update(args)
        self.sock.settimeout(timeout if timeout is not None else 30.0)
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while True:
            line = self._readline()
            reply = json.loads(line)
            if reply.get("id") == self._id:
                if not reply.get("ok"):
                    raise DriveError(reply)
                return reply

    def _readline(self):
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("drive socket closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode()

    # --- commands ----------------------------------------------------------------------------
    def ping(self):
        return self.call("ping")

    def state(self):
        return self.call("state")["state"]

    def targets(self):
        return self.call("targets")["targets"]

    def screenshot(self, path):
        want_png = path.lower().endswith(".png")
        bmp = path[:-4] + ".bmp" if want_png else path
        os.makedirs(os.path.dirname(os.path.abspath(bmp)), exist_ok=True)
        self.call("screenshot", path=os.path.abspath(bmp))
        if want_png:
            subprocess.run(["sips", "-s", "format", "png", bmp, "--out", path], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.remove(bmp)
        return path

    def type(self, text):
        return self.call("type", text=text)

    def key(self, key, mods=()):
        return self.call("key", key=key, mods=list(mods))

    def click(self, target=None, x=None, y=None, times=1):
        if target is not None:
            return self.call("click", target=target, times=times)
        return self.call("click", x=x, y=y, times=times)

    def drag(self, frm, to):
        return self.call("drag", **{"from": frm, "to": to})

    def scroll(self, target, dy, dx=0.0):
        return self.call("scroll", target=target, dy=dy, dx=dx)

    def wait(self, until="settle", text=None, timeout_ms=30000, frames=3):
        args = {"until": until, "timeout_ms": timeout_ms, "frames": frames}
        if text is not None:
            args["text"] = text
        return self.call("wait", timeout=timeout_ms / 1000.0 + 10.0, **args)

    def drop(self, path=None, text=None, target="input"):
        """Drop a file / folder path or text onto the current terminal (as Finder would)."""
        args = {"target": target}
        if path:
            args["path"] = path
        if text:
            args["text"] = text
        return self.call("drop", **args)

    def attach(self, url, token=None, token_file=None):
        args = {"url": url}
        if token:
            args["token"] = token
        if token_file:
            args["token_file"] = os.path.abspath(token_file)
        return self.call("attach", **args)

    def resize(self, w, h):
        return self.call("resize", w=int(w), h=int(h))

    def focus(self):
        return self.call("focus")

    def clipboard(self, text=None):
        args = {} if text is None else {"text": text}
        return self.call("clipboard", **args)["text"]

    def label_case(self, which):
        return self.call("label-case", **{"case": which})

    def panes(self):
        return self.call("panes")["panes"]

    def pane_open(self, kind="", target="", path="", new_pane=False):
        return self.call("pane-open", kind=kind, target=target, path=path, **{"new-pane": new_pane})

    def pane_act(self, action, **args):
        return self.call("pane-act", action=action, args=args)

    def quit(self):
        try:
            return self.call("quit")
        finally:
            self.close()


# --- CLI ---------------------------------------------------------------------------------------
ARITY = {"screenshot": 1, "state": 0, "targets": 0, "ping": 0, "type": 1, "key": 1, "click": 1,
         "drag": 2, "scroll": 2, "wait": 1, "attach": 2, "focus": 0, "quit": 0, "drop": 1, "resize": 2}


def main(argv):
    if len(argv) < 3 or argv[1] != "--socket":
        print(__doc__)
        return 2
    sock = argv[2]
    i = 3
    d = Drive(sock)
    try:
        while i < len(argv):
            cmd = argv[i]
            i += 1
            if cmd not in ARITY:
                print(f"unknown command {cmd}", file=sys.stderr)
                return 2
            args = argv[i:i + ARITY[cmd]]
            i += ARITY[cmd]
            opts = {}
            while i < len(argv) and argv[i].startswith("--"):
                name = argv[i][2:]
                opts[name] = argv[i + 1] if i + 1 < len(argv) else ""
                i += 2
            if cmd == "screenshot":
                print(d.screenshot(args[0]))
            elif cmd == "state":
                print(json.dumps(d.state(), indent=1))
            elif cmd == "targets":
                print("\n".join(d.targets()))
            elif cmd == "ping":
                print(json.dumps(d.ping()))
            elif cmd == "drop":
                r = d.drop(path=args[0], target=opts.get("target", "input"))
            elif cmd == "type":
                d.type(args[0])
            elif cmd == "key":
                mods = [m for m in opts.get("mods", "").split(",") if m]
                d.key(args[0], mods)
            elif cmd == "click":
                if "," in args[0] and args[0][0].isdigit():
                    x, y = args[0].split(",")
                    r = d.click(x=float(x), y=float(y))
                else:
                    r = d.click(args[0], times=int(opts.get("times", "1")))
                print(json.dumps(r.get("at")))
            elif cmd == "drag":
                print(json.dumps(d.drag(args[0], args[1])))
            elif cmd == "scroll":
                d.scroll(args[0], float(args[1]))
            elif cmd == "wait":
                r = d.wait(args[0], text=opts.get("text"), timeout_ms=int(opts.get("timeout", "30000")))
                print(f"{args[0]} after {r.get('elapsed_ms')} ms")
            elif cmd == "attach":
                d.attach(args[0], token_file=args[1])
            elif cmd == "focus":
                d.focus()
            elif cmd == "resize":
                d.resize(args[0], args[1])
            elif cmd == "quit":
                d.quit()
                return 0
        return 0
    except DriveError as e:
        print(f"error: {e} {json.dumps({k: v for k, v in e.reply.items() if k not in ('ok', 'err', 'state')})}",
              file=sys.stderr)
        return 1
    finally:
        d.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
