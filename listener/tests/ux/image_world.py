#!/usr/bin/env python3
"""Images on Messages, on a real world and client: a file dropped on a chat, the Model that takes
images seeing it, the row in the transcript, the image module, a Model that takes none, and a
file picked from the world.

    privateworld.py --port 7842 -- python3 image_world.py --listener <binary> --url {url} \
        --token-file {token} --root-token {root} --shots <dir>
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, HERE)
from drive import Drive  # noqa: E402
import privateworld  # noqa: E402


def until(what, f, seconds=45.0):
    end = time.time() + seconds
    while time.time() < end:
        v = f()
        if v:
            return v
        time.sleep(0.2)
    raise AssertionError("never: " + what)


def set_images(c, model, on):
    r = c.req("set-field", {"object": model, "field": "model/images", "type": "boolean", "value": on, "reader": "both"})
    assert r.get("ok"), r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root-token", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-i-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    png = os.path.join(HERE, "..", "..", "assets", "server-96.png")
    port = int(a.url.rsplit(":", 1)[1].split("/")[0])
    root = privateworld.Client(port, open(a.root_token).read().strip())
    model = root.req("resolve", {"name": "root.model", "op": "read"})["result"]["object"]
    set_images(root, model, True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "image.log"), "w"))

    def shot(name):
        d.wait("settle", frames=4)
        d.screenshot(os.path.join(shots, name + ".png"))

    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        until("the fold", lambda: "principal:root" in d.targets())
        d.click("principal:root")
        d.wait("settle", frames=6)

        # dropped: a chip, then it goes with the line, the Model sees it, and the row shows it
        d.drop(path=os.path.realpath(png))
        until("the chip", lambda: "pending-image:server-96.png" in d.targets())
        time.sleep(0.5)
        d.type("what is this")
        d.key("return")
        until("the row", lambda: "image:server-96.png" in d.targets())
        d.wait(until="text", text="I see 1 image", timeout_ms=30000)
        shot("sent")
        assert "pending-image:server-96.png" not in d.targets(), "the chip goes with the line"

        # a click opens it in the image module
        d.click("image:server-96.png")
        until("the pane", lambda: "PANES" in [w["name"] for w in d.state()["windows"] if not w.get("hidden")])
        shot("opened")
        d.click("tab:chat")
        d.wait("settle", frames=4)

        # a Model that takes none is told so, and the person sees that it went as text
        set_images(root, model, False)
        d.drop(path=os.path.realpath(png))
        until("the chip again", lambda: "pending-image:server-96.png" in d.targets())
        time.sleep(0.5)
        d.click("input")
        d.type("and this one")
        d.key("return")
        d.wait(until="text", text="I am root", timeout_ms=30000)
        shot("held")

        # picked from the world
        project = os.path.join(os.environ["ORBIS_WORLD_DATA"], "grid-project")
        os.makedirs(project, exist_ok=True)
        shutil.copy(png, os.path.join(project, "picked.png"))
        d.click("button:attach-file")
        until("the dialog", lambda: "input:open-file" in d.targets())
        d.click("input:open-file")
        d.type(os.path.join(project, "picked.png"))
        d.key("return")
        until("the picked chip", lambda: "pending-image:picked.png" in d.targets())
        shot("picked")
        print("[image] ok")
    finally:
        try:
            d.quit()
        except Exception:
            pass
        proc.terminate()
        root.close()


if __name__ == "__main__":
    sys.exit(main())
