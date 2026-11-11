#!/usr/bin/env python3
"""A real world behind PANES: every module, typed through the listener, confirmed on disk.

    privateworld.py --port 7824 -- panes_world.py --listener <binary> --url {url} --token-file {token}

vim, less and htop on a world-side PTY, in the box, where a setuid program like top cannot start, the modes those need, the file manager's verbs, an
image, a markdown viewer following its file, a TeX page rendered by the world, an editor saving to
disk, the layout surviving a restart and a second client showing the same terminal. Exit 0 on
pass, 1 on failure, 77 when no world answers.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402


def until(what, fn, seconds=20.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.2)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--target", default="grid.shell")
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    u = urlparse(a.url)
    try:
        socket.create_connection((u.hostname, u.port), timeout=2).close()
    except OSError:
        print("[world] no world at", a.url)
        return 77
    tmp = tempfile.mkdtemp(prefix="orbis-w-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    project = os.path.join(os.environ["ORBIS_WORLD_DATA"], "grid-project")
    with open(os.path.join(project, "long.txt"), "w") as f:
        f.write("".join(f"line {i}\n" for i in range(1, 400)))
    with open(os.path.join(project, "notes.txt"), "w") as f:
        f.write("first line\nsecond line\n")
    with open(os.path.join(project, "gone.txt"), "w") as f:
        f.write("x")
    with open(os.path.join(project, "paper.tex"), "w") as f:
        f.write("\\documentclass{article}\n\\begin{document}\nHello: $e^{i\\pi}+1=0$.\n\\end{document}\n")
    shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "server.png"), project)

    def client(name):
        home = os.path.join(tmp, name)
        os.makedirs(home)
        sock = os.path.join(home, "d.sock")
        proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                                 "--data-dir", os.path.join(home, "data")],
                                stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, name + ".log"), "w"))
        drv = Drive(sock)
        drv.wait("attached", timeout_ms=30000)
        drv.resize(1280, 800)
        return proc, drv

    ui = None
    second = None
    d = None

    def live(kind):
        return next((p for p in d.panes()["live"] if p.get("kind") == kind), None)

    def screen():
        p = live("terminal")
        return p.get("text", "") if p else ""

    def body():
        return next(t for t in d.targets() if t.startswith("panebody:") and "terminal" in t)

    def in_terminal(line):
        d.type(line)
        d.key("Return")

    def right(name):
        d.call("click", target="file:" + name, button="right")
        d.wait("settle", frames=4)

    def verb(name, word, answer=None):
        right(name)
        d.click("menu:" + word)
        d.wait("settle", frames=4)
        if answer is None:
            d.click("button:files-go")
        else:
            d.click("input:files")
            d.wait("settle", frames=3)
            d.key("a", ["cmd"])
            d.type(answer)
            d.key("Return")

    try:
        ui, d = client("first")
        d.click("tab:panes")
        d.wait("settle", frames=6)
        d.pane_open(kind="terminal", target=a.target)
        until("a terminal pane", lambda: live("terminal") or d.panes()["error"])
        assert not d.panes()["error"], d.panes()["error"]
        until("a prompt", lambda: screen().strip())
        d.wait("settle", frames=6)
        d.click("panebody:" + live("terminal")["title"])

        # vim: the file reaches the disk
        in_terminal("vim -u NONE vimmed.txt")
        until("vim's screen", lambda: "~" in screen() and live("terminal")["alt"])
        d.type("ihello through the world")
        d.key("Escape")
        until("the text on screen", lambda: "hello through the world" in screen())
        d.screenshot(os.path.join(shots, "vim.png"))
        d.type(":wq")
        d.key("Return")
        until("vim's exit", lambda: not live("terminal")["alt"])
        path = os.path.join(project, "vimmed.txt")
        until("the file on disk", lambda: os.path.exists(path) and "hello through the world" in open(path).read())

        # less: an arrow key arrives as the application cursor key it asked for
        in_terminal("less long.txt")
        until("less", lambda: "line 1" in screen() and live("terminal")["alt"])
        first = screen().split("\n")[0]
        d.key("Down")
        d.key("Down")
        until("Down scrolls", lambda: screen().split("\n")[0] != first, 6.0)
        d.screenshot(os.path.join(shots, "less.png"))
        d.type("q")
        until("less's exit", lambda: not live("terminal")["alt"])
        for prog in ("htop",):
            if shutil.which(prog) is None:
                print("[world] no", prog)
                continue
            in_terminal(prog)
            until(prog, lambda: "PID" in screen() and live("terminal")["alt"])
            time.sleep(1.0)
            d.screenshot(os.path.join(shots, prog + ".png"))
            d.type("q")
            until(prog + "'s exit", lambda: not live("terminal")["alt"])

        # modes a program sets reach the client, and clear again
        in_terminal("printf '\\033[?1000;1006h'")
        until("mouse reporting", lambda: live("terminal")["mouse"], 6.0)
        in_terminal("printf '\\033[?1000;1006l'")
        until("mouse off", lambda: not live("terminal")["mouse"], 6.0)
        # a click reaches a real program that asked for the mouse, in its own encoding
        in_terminal("printf '\\033[?1000;1006h'; cat -v")
        until("mouse reporting for cat", lambda: live("terminal")["mouse"], 6.0)
        d.click("panebody:" + live("terminal")["title"])
        until("the click in cat", lambda: "[<0;" in screen(), 6.0)
        d.key("c", ["ctrl"])
        in_terminal("printf '\\033[?1000;1006l'")
        until("mouse off again", lambda: not live("terminal")["mouse"], 6.0)
        until("attached names this client", lambda: d.panes()["live"], 2.0)

        # the file manager's verbs, from the menu on a row: a click opens
        d.pane_open(kind="files", target="grid.fs", new_pane=True)
        until("a listing", lambda: live("files") and "notes.txt" in live("files")["panels"][0]["entries"])
        d.wait("settle", frames=6)
        names = lambda: sorted(os.listdir(project))  # noqa: E731
        verb("notes.txt", "copy", "copied.txt")
        until("copy", lambda: "copied.txt" in names())
        d.wait("settle", frames=6)
        verb("copied.txt", "rename", "renamed.txt")
        until("rename", lambda: "renamed.txt" in names() and "copied.txt" not in names())
        d.wait("settle", frames=6)
        os.makedirs(os.path.join(project, "sub"), exist_ok=True)
        d.click("button:files-refresh")
        d.wait("settle", frames=6)
        verb("renamed.txt", "move", "sub/moved.txt")
        until("move", lambda: os.path.exists(os.path.join(project, "sub", "moved.txt")))
        d.wait("settle", frames=6)
        verb("gone.txt", "delete")
        until("delete", lambda: "gone.txt" not in names())
        d.wait("settle", frames=6)
        d.screenshot(os.path.join(shots, "files.png"))

        # an image, by chunks
        d.click("file:server.png")
        until("the image", lambda: (live("image") or {}).get("w") or (live("image") or {}).get("status"))
        assert live("image")["w"] > 0, live("image")
        d.wait("settle", frames=6)
        d.screenshot(os.path.join(shots, "image.png"))
        d.click("close:server.png")
        d.wait("settle", frames=6)

        # a viewer follows its file
        d.click("file:readme.md" if "file:readme.md" in d.targets() else "file:README.md")
        until("markdown", lambda: live("markdown") and live("markdown")["text"])
        with open(os.path.join(project, "README.md"), "a") as f:
            f.write("\nappended on disk\n")
        until("live reload", lambda: "appended on disk" in live("markdown")["text"], 8.0)
        d.wait("settle", frames=6)
        d.screenshot(os.path.join(shots, "markdown.png"))

        # TeX: the world renders, the client draws the page
        d.pane_open(path="paper.tex", target="grid.fs")
        until("tex", lambda: live("latex") and live("latex")["text"])
        d.click("button:doc-render")
        until("a page or an answer", lambda: live("latex")["page"] or (live("latex")["status"] and "rendering" not in live("latex")["status"]), 200.0)
        if live("latex")["page"]:
            d.wait("settle", frames=6)
            d.screenshot(os.path.join(shots, "tex-page.png"))
        else:
            assert shutil.which("tectonic") is None, live("latex")
            print("[world] no tectonic:", live("latex")["status"])
        d.click("close:paper.tex")
        d.wait("settle", frames=6)

        # the editor saves to disk
        d.click("tab:grid-project")
        d.wait("settle", frames=6)
        d.click("file:notes.txt")
        until("the editor", lambda: live("editor") and "first line" in live("editor")["text"])
        d.click("panebody:notes.txt")
        d.wait("settle", frames=4)
        d.type("typed ")
        until("the local edit", lambda: "typed" in live("editor")["text"])
        d.key("s", ["cmd"])
        until("the save", lambda: "typed" in open(os.path.join(project, "notes.txt")).read())

        # the layout is a Field: a client that quits comes back to its panes
        time.sleep(2.0)  # the layout saves on a short delay
        d.quit()
        d = None
        ui.wait(10)
        ui, d = client("again")
        until("its tab back", lambda: "tab:terminal" in d.targets() or any(t.startswith("tablist:") for t in d.targets()), 10.0)
        if "tab:terminal" in d.targets():
            d.click("tab:terminal")
        else:
            d.click(next(t for t in d.targets() if t.startswith("tablist:")))
            d.wait("settle", frames=4)
            d.click("tablist-item:terminal")
        until("the terminal back", lambda: live("terminal"), 10.0)
        until("the editor back", lambda: live("editor"), 10.0)

        # a second client shows the same terminal, live
        second, b = client("second")
        until("its tab", lambda: "tab:terminal" in b.targets() or any(t.startswith("tablist:") for t in b.targets()), 10.0)
        if "tab:terminal" in b.targets():
            b.click("tab:terminal")
        else:
            b.click(next(t for t in b.targets() if t.startswith("tablist:")))
            b.wait("settle", frames=4)
            b.click("tablist-item:terminal")
        until("the same terminal", lambda: next((p for p in b.panes()["live"] if p.get("kind") == "terminal"), None), 10.0)
        d.click(next(t for t in d.targets() if t.startswith("tab:terminal")))
        d.wait("settle", frames=4)
        d.click(body())
        d.wait("settle", frames=4)
        in_terminal("echo two-clients")
        until("the second client sees it",
              lambda: "two-clients" in next(p for p in b.panes()["live"] if p.get("kind") == "terminal").get("text", ""), 8.0)
        b.quit()
        second.wait(10)

        # a shell is no filesystem mount: the world says so, and the client shows its answer
        d.pane_open(kind="files", target=a.target, new_pane=True)
        until("the world's answer", lambda: "not a filesystem mount" in d.panes()["error"], 10.0)
        print("[world] pass; screenshots in", shots)
        return 0
    except Exception as e:  # noqa: BLE001
        print("[world] FAIL", e)
        if d:
            try:
                d.screenshot(os.path.join(shots, "world-failure.png"))
                print(json.dumps(d.panes())[:1200])
            except Exception:  # noqa: BLE001
                pass
        return 1
    finally:
        if d:
            try:
                d.quit()
            except Exception:  # noqa: BLE001
                pass
        for proc in (ui, second):
            if proc:
                proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
