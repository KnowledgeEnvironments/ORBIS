#!/usr/bin/env python3
"""Hold TALK: the words land in the line and nothing is sent until the person sends it. SPEAK asks for
speech on request. A fake OpenAI-compatible endpoint stands in for the Model, and ORBIS_FAKE_MIC for
the microphone, so this says nothing about a real one.

    privateworld.py --port 7839 -- voice_world.py --listener <binary> --url {url} --token-file {token}
                                                  --root-token {root} --shots <dir>
Exit 0 on pass, 1 on failure.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drive import Drive  # noqa: E402
from privateworld import Client  # noqa: E402

SAID = "the words I said"


def silence(ms=200, rate=16000):
    data = b"\0\0" * (rate * ms // 1000)
    return b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data


class Speech(BaseHTTPRequestHandler):
    calls = []

    def log_message(self, *a):
        pass

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("content-type", kind)
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.reply(json.dumps({"data": [{"id": "whisper"}]}).encode(), "application/json")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        Speech.calls.append((self.path, body))
        if self.path.endswith("/audio/transcriptions"):
            self.reply(json.dumps({"text": SAID}).encode(), "application/json")
        else:
            self.reply(silence(), "audio/wav")


def until(what, fn, seconds=20.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.15)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root-token", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-v-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    mic = os.path.join(tmp, "mic.wav")
    open(mic, "wb").write(silence())
    server = ThreadingHTTPServer(("127.0.0.1", 0), Speech)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/v1"
    port = urlparse(a.url).port
    root = Client(port, open(a.root_token).read().strip())
    r = root.req("act", {"action": "model.create", "args": {"name": "voice", "url": url, "model-id": "m", "stt": "whisper-1", "tts": "tts-1"}})
    assert r.get("ok"), r
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")], env=dict(os.environ, ORBIS_FAKE_MIC=mic),
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "listener.log"), "w"))
    try:
        d = Drive(sock)
        d.wait("attached", timeout_ms=30000)
        d.wait("route", timeout_ms=30000)
        d.resize(1280, 800)
        d.wait("settle", frames=10, timeout_ms=8000)
        d.click("principal:root")
        d.wait("settle", frames=10, timeout_ms=8000)
        until("TALK shows", lambda: "button:talk" in d.targets())
        d.click("button:talk")
        until("the transcription call", lambda: any(p.endswith("/audio/transcriptions") for p, _ in Speech.calls))
        until("the words in the line", lambda: SAID in d.state().get("input_text", ""))
        d.wait("settle", frames=8, timeout_ms=8000)
        d.screenshot(os.path.join(shots, "talk.png"))
        assert SAID not in json.dumps(root.req("history", {"last": 200}).get("result", {})), "nothing is sent by itself"
        # read it, then send it by hand; the agent answers, and SPEAK asks for that answer
        d.click("input")
        d.key("Return")
        until("root's reply", lambda: "I am root" in json.dumps(d.state()), 30)
        d.wait("settle", frames=8, timeout_ms=8000)
        d.call("click", x=850, y=560, button="right")
        d.wait("settle", frames=6, timeout_ms=8000)
        d.screenshot(os.path.join(shots, "menu.png"))
        d.click("menu:speak")
        until("the speech call", lambda: any(p.endswith("/audio/speech") for p, _ in Speech.calls))
        assert b"I am root" in next(b for p, b in Speech.calls if p.endswith("/audio/speech"))
        print("[voice] ok")
        return 0
    except AssertionError:
        d.screenshot(os.path.join(shots, "fail.png"))
        print("input:", repr(d.state().get("input_text")), [c[0] for c in Speech.calls])
        raise
    finally:
        proc.terminate()
        server.shutdown()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print(f"[voice] FAIL {e}")
        sys.exit(1)
