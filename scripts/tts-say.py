#!/usr/bin/env python3
"""Text to speech for a world, from the Mac's own voices: an OpenAI-shaped /v1/audio/speech on a port,
so a Model with `tts` can point at it (docs/panes.md Voice; speech.say in the world).
   scripts/tts-say.py [--port 8091] [--voice Samantha] [--host 127.0.0.1]
GET /v1/models answers one model, `say`, so the world's probe says ok. POST /v1/audio/speech takes
{input, voice?, response_format?} and answers 16-bit mono WAV at 22050 Hz, through `say` and
`afconvert`. Nothing is kept: each request speaks into a temp file that is removed after the answer.
Standard library only; macOS only (it is the Mac's speech)."""
import argparse, json, os, subprocess, tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VOICE = "Samantha"
MAX_CHARS = 4000

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _json(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data))); self.end_headers(); self.wfile.write(data)

    def do_GET(self):
        if self.path.rstrip("/") in ("/v1/models", "/models"):
            return self._json(200, {"object": "list", "data": [{"id": "say", "object": "model", "owned_by": "macos"}]})
        self._json(404, {"error": "not here"})

    def do_POST(self):
        if not self.path.rstrip("/").endswith("/audio/speech"):
            return self._json(404, {"error": "not here"})
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("content-length", "0")) or b"{}"))
        except ValueError:
            return self._json(400, {"error": "json body expected"})
        text = str(req.get("input") or "")[:MAX_CHARS].strip()
        if not text:
            return self._json(400, {"error": "input is empty"})
        voice = str(req.get("voice") or VOICE)
        if not voice.replace(" ", "").replace("-", "").isalnum():
            voice = VOICE
        with tempfile.TemporaryDirectory() as d:
            aiff = os.path.join(d, "say.aiff"); wav = os.path.join(d, "say.wav")
            try:
                subprocess.run(["say", "-v", voice, "-o", aiff, text], check=True, timeout=120, capture_output=True)
                subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@22050", "-c", "1", aiff, wav], check=True, timeout=60, capture_output=True)
                data = open(wav, "rb").read()
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as e:
                return self._json(500, {"error": "say failed: " + str(e)[:200]})
        self.send_response(200); self.send_header("content-type", "audio/wav")
        self.send_header("content-length", str(len(data))); self.end_headers(); self.wfile.write(data)

def main():
    global VOICE
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=8091)
    ap.add_argument("--host", default="127.0.0.1"); ap.add_argument("--voice", default=VOICE)
    a = ap.parse_args()
    VOICE = a.voice
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()

if __name__ == "__main__":
    main()
