#!/usr/bin/env python3
"""The Model control and Sign in with ChatGPT through the real client, against a stub of OpenAI's
documented endpoints. No account and no network: the world reaches the stub through KE_OPENAI_AUTH and
KE_OPENAI_API, and this script plays the browser, calling the client's own loopback listener.

    privateworld.py --port 7839 -- model_world.py --listener <binary> --url {url} --token-file {token}
                                                  --stub-port 7897 --shots <dir>

Exit 0 on pass, 1 on failure.
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402


def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


class Keys:
    def __init__(self, tmp):
        self.pem = os.path.join(tmp, "k.pem")
        subprocess.run(["openssl", "genrsa", "-out", self.pem, "2048"], check=True, capture_output=True)
        out = subprocess.run(["openssl", "rsa", "-in", self.pem, "-noout", "-modulus"], check=True,
                             capture_output=True, text=True).stdout
        self.n = b64(bytes.fromhex(out.strip().split("=")[1]))

    def sign(self, data):
        return subprocess.run(["openssl", "dgst", "-sha256", "-sign", self.pem], input=data, check=True,
                              capture_output=True).stdout


class Stub:
    nonce = None
    keys = None
    base = None
    bodies = []
    bearers = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body, kind="application/json"):
        data = body.encode()
        self.send_response(code)
        self.send_header("content-type", kind)
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/.well-known"):
            self.reply(200, json.dumps({"jwks_uri": Stub.base + "/jwks", "revocation_endpoint": Stub.base + "/revoke"}))
        elif self.path == "/jwks":
            self.reply(200, json.dumps({"keys": [{"kid": "k", "kty": "RSA", "n": Stub.keys.n, "e": "AQAB"}]}))
        elif self.path == "/v1/models":
            Stub.bearers.append(self.headers.get("authorization"))
            self.reply(200, json.dumps({"models": [{"slug": "gpt-a", "display_name": "GPT A", "visibility": "list"},
                                                   {"slug": "secret", "display_name": "No", "visibility": "hide"}]}))
        else:
            self.reply(404, "{}")

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", 0)))
        if self.path.endswith("/token"):
            head = b64(json.dumps({"alg": "RS256", "kid": "k"}).encode())
            claims = b64(json.dumps({"iss": Stub.base, "aud": "oaiapp_ui", "sub": "u1", "email": "a@example.com",
                                     "exp": int(time.time()) + 3600, "nonce": Stub.nonce}).encode())
            sig = b64(Stub.keys.sign((head + "." + claims).encode()))
            self.reply(200, json.dumps({
                "access_token": "stub-access", "refresh_token": "stub-refresh", "token_type": "Bearer",
                "expires_in": 3600, "id_token": head + "." + claims + "." + sig,
                "scope": "chatgpt.tokens.use.direct email offline_access openid profile resource.invoke"}))
        elif self.path == "/v1/responses":
            Stub.bodies.append(json.loads(raw))
            Stub.bearers.append(self.headers.get("authorization"))
            events = [{"type": "response.output_text.delta", "delta": "hello from the plan"},
                      {"type": "response.completed", "response": {"model": "gpt-a", "usage": {"input_tokens": 3, "output_tokens": 4}}}]
            self.reply(200, "".join("data: " + json.dumps(e) + "\n\n" for e in events), "text/event-stream")
        else:
            self.reply(200, "")


def until(what, check, seconds=30.0):
    end = time.time() + seconds
    while time.time() < end:
        v = check()
        if v:
            return v
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--stub-port", type=int, required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-m-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    Stub.keys = Keys(tmp)
    Stub.base = "http://127.0.0.1:%d" % a.stub_port
    server = ThreadingHTTPServer(("127.0.0.1", a.stub_port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "model.log"), "w"))
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("root", lambda: "principal:root" in d.targets(), 60.0)
        d.click("principal:root")
        until("the Model control", lambda: "button:model" in d.targets())

        # the control lists every Model's offers, with what each can do
        d.click("button:model")
        until("the list", lambda: any(t.startswith("menu:model:") for t in d.targets()))
        time.sleep(0.3)
        d.screenshot(os.path.join(shots, "picker.png"))
        d.click("menu:model-settings")
        until("the settings", lambda: "button:model-scope-agent" in d.targets())

        # Sign in with ChatGPT: the client opens nothing under drive, so this script is the browser
        d.click("button:signin")
        url = until("the address", lambda: d.state().get("signin", {}).get("url"))
        q = {k: v[0] for k, v in urllib.parse.parse_qs(urllib.parse.urlparse(url).query).items()}
        assert q["client_id"] == "dynamic_agent_client", q
        assert q["agent_name_hint"] == "ORBIS", q
        assert "chatgpt.tokens.use.direct" in q["scope"], q
        Stub.nonce = q["nonce"]
        back = q["redirect_uri"] + "?" + urllib.parse.urlencode(
            {"code": "ui-code", "state": q["state"], "client_id": "oaiapp_ui"})
        with urllib.request.urlopen(back, timeout=10) as r:
            assert r.status == 200 and b"Signed in" in r.read()
        until("signed in", lambda: d.state().get("signin", {}).get("status") == "signed in")
        until("the welcome", lambda: "button:signin-got-it" in d.targets())
        d.screenshot(os.path.join(shots, "welcome.png"))
        d.click("button:signin-got-it")
        time.sleep(0.3)

        # the new Model's real catalogue is offered, and the hidden model is not
        d.key("escape")
        d.click("button:model")
        row = until("the plan's model", lambda: next((t for t in d.targets() if t.startswith("menu:model:chatgpt:gpt-a")), None))
        assert not any("secret" in t for t in d.targets()), "a hidden model was offered"
        d.click(row)
        until("the plan in use", lambda: "button:manage-usage" in d.targets())
        time.sleep(0.3)
        d.screenshot(os.path.join(shots, "plan.png"))

        # a real wake through the Responses route, with the token only in the header
        d.click("input")
        d.type("hello")
        d.key("return")
        until("the request", lambda: Stub.bodies, 30.0)
        body = Stub.bodies[0]
        assert body["store"] is False and body["stream"] is True and body["model"] == "gpt-a", body
        assert "Bearer stub-access" in Stub.bearers, Stub.bearers
        time.sleep(1.0)
        d.screenshot(os.path.join(shots, "replied.png"))
        print("[model] ok")
        return 0
    except Exception as e:  # noqa: BLE001
        print("[model] FAIL", repr(e))
        try:
            d.screenshot(os.path.join(shots, "failed.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        proc.terminate()
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
