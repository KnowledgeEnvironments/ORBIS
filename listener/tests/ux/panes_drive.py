#!/usr/bin/env python3
"""The PANES area through the drive API, against mockworld's real PTY and a real directory.

    panes_drive.py --listener <binary> --mock <mockworld> [--port 7931] [--shots DIR]

Runs vim, less, top and htop in a terminal pane and confirms the file vim saved; opens the files,
editor, markdown and tex modules; closes a dirty editor through its confirm. Exit 0 on pass.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402

MD = """# Notes

Inline $\\frac{a}{b} + \\sqrt{x}$ math costs $5 not $6.

$$\\sum_{i=0}^{n} \\alpha_i = \\int_0^1 f(x)\\,dx$$

```tex
\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}
```

- a list
"""
TEX = r"""\documentclass{article}
\title{A Paper}
\begin{document}
\maketitle
\section{Start}
Energy is \textbf{conserved}, $E = mc^2$.
\begin{equation}
\frac{\partial u}{\partial t} = \nabla^2 u
\end{equation}
\end{document}
"""


def until(what, fn, seconds=15.0):
    end = time.time() + seconds
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(0.15)
    raise AssertionError(f"timed out: {what} (last {last!r})")


def live(d, kind):
    for p in d.panes()["live"]:
        if p.get("kind") == kind:
            return p
    return None


def pick(d, name):
    """A tab by name; past what fits, through the list that names them all."""
    if ("tab:" + name).lower() in (t.lower() for t in d.targets()):  # the drive lists names in lower case
        d.click("tab:" + name)
        return
    d.click("tablist:leaf")
    d.wait("settle", frames=4)
    d.click("tablist-item:" + name)


def text_of(d, kind="terminal"):
    p = live(d, kind)
    return p.get("text", "") if p else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--port", type=int, default=7931)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-panes-")
    root = os.path.join(tmp, "root")
    os.makedirs(os.path.join(root, "sub"))
    with open(os.path.join(root, "README.md"), "w") as f:
        f.write(MD)
    with open(os.path.join(root, "paper.tex"), "w") as f:
        f.write(TEX)
    with open(os.path.join(root, "notes.txt"), "w") as f:
        f.write("first line\nsecond line\n")
    shots = a.shots or os.path.join(tmp, "shots")
    os.makedirs(shots, exist_ok=True)
    env = dict(os.environ, LISTENER_MOCK_ROOT=root)
    mock = subprocess.Popen([a.mock, "--port", str(a.port), "--period-ms", "400"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sock = os.path.join(tmp, "d.sock")
    ui = subprocess.Popen([a.listener, "--url", f"ws://127.0.0.1:{a.port}/ws", "--token", "t", "--drive", sock,
                           "--data-dir", os.path.join(tmp, "data")], stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "listener.log"), "w"))
    d = None
    try:
        d = Drive(sock)
        d.wait("attached", timeout_ms=20000)
        d.resize(1560, 860)
        d.click("tab:panes")
        d.wait("settle")

        # FIELDS holds still with hostile values, wide and narrow
        d.click("tab:chat")
        d.wait("settle", frames=8)
        d.click(next(t for t in d.targets() if t.startswith("principal:")))
        d.wait("settle", frames=8)
        d.click("tab:fields")
        d.wait("settle", frames=8)
        d.screenshot(os.path.join(shots, "fields.png"))
        d.resize(760, 640)
        d.wait("settle", frames=8)
        d.screenshot(os.path.join(shots, "fields-narrow.png"))
        d.resize(1560, 860)
        d.wait("settle", frames=8)

        # a file in a Field: click opens the viewer, shift-click the inspector; reuse, then close both
        link = next((t for t in d.targets() if t.startswith("link:") and "/root/" in t), None)
        assert link, f"no path link in FIELDS: {[t for t in d.targets() if t.startswith('link:')]}"
        d.call("click", target=link, shift=True)
        until("the inspector tab", lambda: any(t.startswith("close:") and "readme" in t.lower() for t in d.targets()))
        d.wait("settle", frames=4)
        d.screenshot(os.path.join(shots, "inspector.png"))
        inspector = next(t for t in d.targets() if t.startswith("close:") and "readme" in t.lower())
        d.click(inspector)
        d.wait("settle", frames=6)
        assert not any(t.startswith("close:") and "readme" in t.lower() for t in d.targets()), "the inspector tab stayed"
        d.click("tab:fields")
        until("the link back", lambda: link in d.targets())
        for attempt in range(3):  # the mock's fold moves rows every 400 ms: a click can land beside the link
            d.wait("settle", frames=6)
            d.click(link)
            d.pane_act("pane.list")
            try:
                until("the viewer", lambda: live(d, "markdown"), 6.0)
                break
            except AssertionError:
                if attempt == 2:
                    raise
        d.wait("settle", frames=4)
        d.click("close:README.md")
        until("the viewer gone", lambda: not live(d, "markdown"))
        print("[panes] click and shift-click ok")
        d.click("tab:panes")
        d.wait("settle")

        # terminal: a real program, real keys, a real file
        d.pane_open(kind="terminal", target="shell")
        until("a terminal pane", lambda: live(d, "terminal"))
        until("a prompt", lambda: "$" in text_of(d))
        body = "panebody:terminal"
        d.wait("settle", frames=4)
        d.click(body)
        d.type("printf '\\033[31mred\\033[0m \\033[48;5;33m256\\033[0m \\033[38;2;255;128;0mtrue\\033[0m\\n'")
        d.key("Return")
        until("colours", lambda: "red 256 true" in text_of(d))
        d.screenshot(os.path.join(shots, "terminal.png"))
        d.type("vim -u NONE vimmed.txt")
        d.key("Return")
        until("vim's alternate screen", lambda: live(d, "terminal")["alt"])
        d.type("ihello from vim")
        d.key("Escape")
        until("vim's text", lambda: "hello from vim" in text_of(d))
        d.screenshot(os.path.join(shots, "vim.png"))
        for attempt in range(3):  # a loaded machine can run Escape and `:` together, which vim reads as Meta
            d.key("Escape")
            d.wait("settle", frames=8)
            d.type(":wq")
            d.key("Return")
            try:
                until("vim's exit", lambda: not live(d, "terminal")["alt"], 6.0)
                break
            except AssertionError:
                if attempt == 2:
                    raise
        saved = os.path.join(root, "vimmed.txt")
        until("the file vim wrote", lambda: os.path.exists(saved) and "hello from vim" in open(saved).read())
        print("[panes] vim wrote", saved)
        for prog, needle, leave in (("less README.md", "Notes", "q"), ("top", "PID", "q"), ("htop", "PID", "q")):
            if shutil.which(prog.split()[0]) is None:
                print("[panes] no", prog)
                continue
            d.type(prog)
            d.key("Return")
            until(prog, lambda: needle in text_of(d) and live(d, "terminal")["alt"])
            d.screenshot(os.path.join(shots, prog.split()[0] + ".png"))
            d.type(leave)
            until(prog + " exit", lambda: not live(d, "terminal")["alt"])
        d.type("for i in $(seq 1 200); do echo line $i; done")
        d.key("Return")
        until("scrollback", lambda: live(d, "terminal")["scrollback"] > 100)

        # a model's input is marked on the pane
        pane = live(d, "terminal")["pane"]
        d.pane_act("pane.model-send", pane=pane, text="echo from the model\n")
        until("the model's echo", lambda: "from the model" in text_of(d))
        d.wait("settle")
        d.screenshot(os.path.join(shots, "model-input.png"))

        # files
        d.pane_open(kind="files", target="shell", new_pane=True)
        until("a listing", lambda: live(d, "files") and "README.md" in live(d, "files")["panels"][0]["entries"])
        d.wait("settle")
        d.screenshot(os.path.join(shots, "files.png"))
        d.click("file:README.md")
        until("the markdown viewer", lambda: live(d, "markdown") and live(d, "markdown")["text"])
        d.wait("settle")
        assert live(d, "markdown")["math"] == 3, live(d, "markdown")
        d.screenshot(os.path.join(shots, "markdown.png"))
        d.pane_open(path="paper.tex", target="shell")
        until("the tex view", lambda: live(d, "latex") and live(d, "latex")["text"])
        d.wait("settle")
        d.screenshot(os.path.join(shots, "tex.png"))
        d.click("button:doc-render")
        until("the world's answer", lambda: "not-implemented" in live(d, "latex")["status"])

        # live reload: the viewer in front follows the file
        pick(d, "README.md")
        d.wait("settle")
        with open(os.path.join(root, "README.md"), "a") as f:
            f.write("\nappended on disk\n")
        until("live reload", lambda: "appended on disk" in live(d, "markdown")["text"], 6.0)

        # editor: type, save, the world holds it; a model's edit shows; a conflict shows
        d.pane_open(path="notes.txt", target="shell")
        until("the editor", lambda: live(d, "editor") and "first line" in live(d, "editor")["text"])
        pick(d, "notes.txt")
        d.click("panebody:notes.txt")
        d.type("typed ")
        until("the local edit", lambda: "typed" in live(d, "editor")["text"])
        d.key("s", ["cmd"])
        until("the save", lambda: "typed" in open(os.path.join(root, "notes.txt")).read())
        ed = live(d, "editor")["pane"]
        d.pane_act("pane.frob", pane=ed, verb="model-insert", args={"at": 0, "text": "MODEL "})
        until("the model's edit", lambda: "MODEL" in live(d, "editor")["text"])
        d.screenshot(os.path.join(shots, "editor.png"))
        d.pane_act("pane.frob", pane=ed, verb="conflict-next", args={})
        d.type("x")
        d.key("s", ["cmd"])
        until("the conflict", lambda: "conflict" in live(d, "editor")["status"])
        d.screenshot(os.path.join(shots, "editor-conflict.png"))
        print("[panes] editor ok")

        # the clipboard, both ways, through every kind of surface
        d.clipboard("from the os")
        d.click("panebody:notes.txt")
        d.key("v", ["cmd"])
        until("paste into the editor", lambda: "from the os" in live(d, "editor")["text"])
        d.key("a", ["cmd"])
        d.key("c", ["cmd"])
        d.wait("settle")
        assert "from the os" in d.clipboard(), d.clipboard()
        pick(d, "terminal")
        d.wait("settle", frames=6)
        d.click("panebody:terminal")
        d.wait("settle", frames=6)
        d.type("printf '\\033[?2004h'; cat > pasted.txt")
        d.key("Return")
        until("bracketed paste on", lambda: live(d, "terminal")["bracketed-paste"])
        d.clipboard("pasted text")
        d.key("v", ["cmd"])
        d.key("Return")
        d.key("d", ["ctrl"])
        pasted = os.path.join(root, "pasted.txt")
        until("the paste, bracketed", lambda: os.path.exists(pasted) and "\033[200~pasted text\033[201~" in open(pasted).read())
        d.type("printf '\\033[?2004l'")
        d.key("Return")
        # the right-click menu on a text field: paste, select all, cut, through the field itself
        pick(d, "notes.txt")
        d.wait("settle", frames=6)
        d.click("button:editor-find")
        d.wait("settle")
        d.clipboard("needle")
        d.call("click", target="input:find", button="right")
        d.wait("settle")
        d.click("menu:paste")
        d.wait("settle", frames=8)
        d.call("click", target="input:find", button="right")
        d.wait("settle")
        d.click("menu:select-all")
        d.wait("settle", frames=8)
        d.clipboard("empty")
        d.call("click", target="input:find", button="right")
        d.wait("settle")
        d.click("menu:cut")
        d.wait("settle", frames=8)
        until("the field's text on the clipboard", lambda: d.clipboard() == "needle", 4.0)
        d.key("Escape")
        d.click("button:find-close")
        print("[panes] clipboard ok")
        # text size, live: larger, a screenshot, back to the default
        for _ in range(4):
            d.key("=", ["cmd"])
            d.wait("settle", frames=4)
        d.wait("settle", frames=8)
        d.screenshot(os.path.join(shots, "font-large.png"))
        d.key("0", ["cmd"])
        d.wait("settle", frames=8)
        for which in ("caps", "sentence"):
            d.label_case(which)
            d.wait("settle", frames=8)
            d.screenshot(os.path.join(shots, f"labels-{which}.png"))
            d.click("tab:chat")
            d.wait("settle", frames=8)
            d.click("tab:fields")
            d.wait("settle", frames=8)
            d.screenshot(os.path.join(shots, f"labels-{which}-fields.png"))
            d.click("tab:panes")
            d.wait("settle", frames=8)
        d.label_case("")
        d.wait("settle", frames=8)
        pick(d, "notes.txt")
        d.wait("settle", frames=6)
        d.click("close:notes.txt")
        d.wait("settle")
        if "button:close-anyway" in d.targets():
            d.click("button:close-anyway")
        until("the tab gone", lambda: not live(d, "editor"))
        print("[panes] pass; screenshots in", shots)
        return 0
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        try:
            d.screenshot(os.path.join(shots, "fail.png"))
            print("[panes] targets", [t for t in d.targets() if t.startswith(("link:", "tab:", "close:"))])
        except Exception:  # noqa: BLE001
            pass
        print("[panes] FAIL", e)
        if d:
            try:
                d.screenshot(os.path.join(shots, "failure.png"))
                print(json.dumps(d.panes())[:1500])
            except Exception:  # noqa: BLE001
                pass
        print("[panes] shots in", shots)
        return 1
    finally:
        if d:
            try:
                d.quit()
            except Exception:  # noqa: BLE001
                pass
        ui.terminate()
        mock.terminate()


if __name__ == "__main__":
    sys.exit(main())
