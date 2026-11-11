#!/usr/bin/env python3
"""The map a person lands on and the notifications, from a world as a new person finds it.

    privateworld.py --port 7836 --no-seed -- map_world.py --listener <binary> --url {url} \
        --token-file {token} --root-token {root} --shots <dir>

Nothing here reaches the main world: privateworld refuses its port. Exit 0 on pass, 1 on failure.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
from drive import Drive  # noqa: E402
from privateworld import Client, seed_grid  # noqa: E402


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=8000)


def until(what, check, seconds=15.0):
    end = time.time() + seconds
    while time.time() < end:
        if check():
            return
        time.sleep(0.08)
    raise AssertionError("timed out waiting for " + what)


def actors(d):
    return {a["name"]: a for a in d.state()["map"]["actors"]}


def places(d):
    return {n: (x["x"], x["y"], x["h"]) for n, x in actors(d).items()}


def no_overlap(d):
    rows = d.state()["map"]["actors"]
    for i, a in enumerate(rows):
        for b in rows[i + 1:]:
            apart = a["x"] + a["w"] <= b["x"] or b["x"] + b["w"] <= a["x"] or a["y"] + a["h"] <= b["y"] or b["y"] + b["h"] <= a["y"]
            assert apart, f"{a['name']} overlaps {b['name']}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--root-token", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-m-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "map.log"), "w"))
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1280, 800)
        settle(d, 10)
        port = urlparse(a.url).port
        root = Client(port, open(a.root_token).read().strip())
        person = Client(port, open(a.token_file).read().strip())
        name_id = lambda n: root.req("resolve", {"name": n, "op": "read"})["result"]["object"]  # noqa: E731

        # the map is what a person lands on, and a new world says what to do first
        until("the map", lambda: "actor:root" in d.targets())
        targets = d.targets()
        assert "actor:person" in targets and "button:welcome-hello" in targets, targets
        assert any(x["kind"] == "model" for x in actors(d).values()), "no Model on the map"
        assert not any(t == "port:transport.listener" for t in targets), "plumbing shown as a port"
        no_overlap(d)
        d.screenshot(os.path.join(shots, "01-welcome.png"))

        d.click("button:welcome-hello")
        settle(d)
        assert "input" in d.targets(), "say hello did not reach a terminal"
        d.click("tab:graph")
        settle(d)

        # a world with work in it
        project_home = os.environ["ORBIS_WORLD_DATA"]
        seed_grid(root, project_home)
        until("grid on the map", lambda: "actor:grid" in d.targets())
        settle(d, 8)
        assert actors(d)["grid"]["depth"] == 1, "a made agent hangs under its maker"
        assert "button:welcome-hello" not in d.targets(), "the first step outstayed the second agent"
        assert any(t.startswith("port:grid.") for t in d.targets()), d.targets()
        no_overlap(d)
        before = places(d)
        d.screenshot(os.path.join(shots, "02-tree.png"))

        # a click opens, a shift-click would inspect, a port inspects
        d.click("actor:root")
        settle(d)
        assert "input" in d.targets(), "a click on an actor did not open its tab"
        d.click("tab:graph")
        settle(d)

        # shift-click inspects, right-click and a long press open its menu, a port inspects
        d.call("click", target="actor:grid", shift=True)
        settle(d)
        assert "tab:graph" in d.targets()
        d.click("tab:graph")
        settle(d)
        d.call("click", target="actor:root", button="right")
        settle(d, 8)
        assert "menu:connect" in d.targets(), "no menu on right-click"
        d.screenshot(os.path.join(shots, "02b-menu.png"))
        d.key("Escape")
        settle(d, 8)
        d.call("click", target="actor:root", hold=40)
        settle(d, 8)
        assert "menu:connect" in d.targets(), "no menu on a long press"
        d.key("Escape")
        settle(d, 8)
        d.click("port:grid.fs")
        settle(d)
        d.click("tab:graph")
        settle(d)

        # a card on the node, decided there; a pulse; the running tape; nothing moves
        before = places(d)  # the inspector the port opened took its share of the width
        rroute = person.req("read", {"object": person.me, "field": "principal/routes"})
        route = rroute["result"]["value"][0] if isinstance(rroute["result"].get("value"), list) else name_id("root.route")
        r = person.req("send", {"to": [name_id("root")], "route": route, "text": "run exactly this command: echo from-the-map."})
        assert r.get("ok"), r
        until("a pulse", lambda: d.state()["map"]["pulsing"], 6.0)
        until("the card on root", lambda: actors(d)["root"]["waiting"] == 1)
        assert actors(d)["root"]["can_decide"], "the person may decide it"
        settle(d)
        d.screenshot(os.path.join(shots, "03-card.png"))
        assert places(d) == before, ("a card moved the map", before, places(d))
        d.click("button:approve:root")
        until("the card gone", lambda: actors(d)["root"]["waiting"] == 0)
        link = [x for x in d.state()["map"]["links"] if {x["from"], x["to"]} == {"person", "root"}]
        assert link and link[0]["traffic"], link
        until("root idle", lambda: not actors(d)["root"]["running"], 12.0)
        r = person.req("send", {"to": [name_id("root")], "route": route, "text": "/think Count slowly to five."})
        assert r.get("ok"), r
        until("the tape", lambda: actors(d)["root"]["running"], 10.0)
        d.screenshot(os.path.join(shots, "04-running.png"))
        assert places(d) == before, "running moved the map"

        # notifications: live, only the loud reach the OS, a badge on node and tab, the window, read
        r = root.act("notify", {"text": "tests are green", "level": "info"})
        assert r.get("ok"), r
        until("the notice", lambda: d.state()["notices"]["unread"] == 1)
        assert d.state()["notices"]["os"] == 0, "an info notice reached the OS"
        grid = name_id("grid")
        r = root.act("notify", {"text": "disk nearly full on grid", "level": "urgent", "object": grid})
        assert r.get("ok"), r
        until("the loud one", lambda: d.state()["notices"]["os"] == 1)
        until("the badge on the node", lambda: actors(d)["root"]["unread"] >= 2)
        settle(d)
        d.screenshot(os.path.join(shots, "05-badge.png"))
        d.click("button:notices")
        settle(d)
        assert "pane:notices" in d.targets()
        d.screenshot(os.path.join(shots, "06-notices.png"))
        rows = d.state()["notices"]["rows"]
        assert rows[0]["text"].startswith("disk"), "newest first"
        d.click("notice:0")
        until("read", lambda: d.state()["notices"]["unread"] == 1)
        settle(d)
        d.click("button:notices")
        settle(d)
        d.click("button:notices")
        settle(d)
        d.click("button:notices-read")
        until("all read", lambda: d.state()["notices"]["unread"] == 0)
        wire = person.act("notify.list", {})
        res = wire["result"].get("result", wire["result"])
        assert res["unread"] == 0, wire
        d.click("button:notices")
        settle(d)

        # a second client attaches and sees what is unread from the world, not from memory
        r = root.act("notify", {"text": "while you were out", "level": "attention"})
        assert r.get("ok"), r
        until("one more", lambda: d.state()["notices"]["unread"] == 1)
        assert d.state()["notices"]["os"] == 2

        # drag an agent onto another: the drop says what can be done
        d.click("tab:graph")
        settle(d)
        d.drag("actor:grid", "actor:root")
        settle(d, 8)
        d.screenshot(os.path.join(shots, "07-drop.png"))
        tg = d.targets()
        assert "menu:drop-connect" in tg and "menu:drop-hand" in tg, tg

        # hand over: a Message to the other agent, from the person, on the person's Route
        d.click("menu:drop-hand")
        settle(d, 8)
        assert "input:handover" in d.targets(), d.targets()
        d.screenshot(os.path.join(shots, "07b-handover.png"))
        d.click("input:handover")
        settle(d)
        d.type("the build is yours")
        settle(d)
        d.click("button:handover-send")
        settle(d, 8)
        until("the hand over sent", lambda: "handed over to root" in d.state()["map"]["status"])

        # two agents that share no Route: CONNECT makes one, through route.create
        r = root.act("principal.create", {"name": "tiny", "charter": "A second worker.",
                                          "grant": {"scope": {"prefix": "tiny"}, "ops": ["read", "write"]}})
        assert r.get("ok"), r
        until("tiny on the map", lambda: "actor:tiny" in d.targets())
        settle(d, 8)
        no_overlap(d)
        joined = lambda: [x for x in d.state()["map"]["links"] if {x["from"], x["to"]} == {"grid", "tiny"}]  # noqa: E731
        assert not joined()
        d.drag("actor:grid", "actor:tiny")
        settle(d, 8)
        d.click("menu:drop-connect")
        until("the new link", joined)
        settle(d, 8)
        d.screenshot(os.path.join(shots, "07c-connected.png"))

        # phone width: stacked, nothing overlaps, every actor still there
        d.resize(390, 800)
        settle(d, 10)
        assert "actor:root" in d.targets(), "a change of layout lost the map"
        assert d.state()["map"]["stacked"], "not stacked at phone width"
        no_overlap(d)
        d.screenshot(os.path.join(shots, "08-narrow.png"))
        d.resize(1280, 800)
        settle(d, 8)
        d.quit()
        print("map: ok")
        return 0
    except Exception as e:  # noqa: BLE001
        print("map: FAIL", repr(e))
        try:
            d.screenshot(os.path.join(shots, "fail.png"))
        except Exception:  # noqa: BLE001
            pass
        return 1
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:  # noqa: BLE001
            proc.kill()
        if not a.shots:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
