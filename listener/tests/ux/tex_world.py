#!/usr/bin/env python3
"""Hard TeX in a chat, on a real world: the line still takes typing afterwards, in every form the
formula arrives in. Exit 0 on pass, 1 on failure.

    privateworld.py --port 7837 -- python3 tex_world.py --listener <binary> --url {url} \
        --token-file {token} --shots <dir>
"""
import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from drive import Drive  # noqa: E402

FORMULA = (r"\displaystyle \mathfrak{O}_{\!\text{RBIS}}=\oint_{\partial\mathcal W}\!\left(\sum_{n=0}^{\infty}"
           r"\frac{(-1)^n\nabla^{2n}}{(2n+1)!}\right)e^{\,i\pi\sqrt{-1}}\;\mathrm d\mu\;\cong\;\left\{\mathcal A"
           r"\mid\forall a\in\mathcal A,\;\Diamond\Box\bigl(\mathrm{Action}(a)\Rightarrow\mathrm{Authorized}(a)"
           r"\bigr)\right\}\rtimes\prod_{\alpha<\omega_1}\mathbb G_\alpha^{\,\aleph_0}\;\xrightarrow[\text{audit}]"
           r"{\;\;\log^\infty\;\;}\;\Omega^{\Omega^{\Omega}}")

REPLIES = ["Behold: $" + FORMULA + "$",
           "```tex<nl>" + FORMULA + "<nl>```",
           "\\mathrm{ORBIS}:\\quad \\forall a,\; \\mathrm{Actions}(a)\\subseteq\\mathrm{Authorized}(a)",
           FORMULA.replace(" ", "<nl>"),
           "$$" + FORMULA + "$$"]


# The exact messages that failed on a real world: a whole document in a fence, then the same content
# as dollars around text, with a failure in among them.
DOCUMENT = (r"\documentclass{article}<nl>\usepackage[margin=1in]{geometry}<nl>\begin{document}<nl>"
            r"\section*{The \texttt{grid} Project}<nl>\subsection*{Scope}<nl>"
            r"Research on \textbf{GRiD Systems Corporation} and the \textbf{GRiD Compass} (the 1980s laptop), "
            r"toward reviving a real Compass~II machine.<nl>\subsection*{Artifacts}<nl>\begin{itemize}<nl>"
            r"\item Floppy dumps of GRiD-OS sources in \texttt{gridsources/} (PL/M, Pascal, 8086 assembly)<nl>"
            r"\end{itemize}<nl>\newpage<nl>A second page.<nl>\end{document}")
FRAGMENT = (r"So here is the same content as a fragment, which should typeset:<nl><nl>$ $<nl>"
            r"$\section*{The grid Project}$<nl>$\subsection*{Scope}$<nl>"
            r"Research on \textbf{GRiD Systems Corporation} and the \textbf{GRiD Compass} (the 1980s laptop), "
            r"toward reviving a real Compass~II machine.<nl>$\subsection*{Artifacts}$<nl>"
            r"- Floppy dumps of GRiD-OS sources in \texttt{gridsources/} (PL/M, Pascal, 8086 assembly)")
FENCE = (r"A fragment in a fence:<nl><nl>```tex<nl>\section*{Fence fragment}<nl>Some \textbf{bold} words.<nl>"
         r"\begin{itemize}<nl>\item one<nl>\end{itemize}<nl>```<nl><nl>and the text goes on")
BROKEN = (r"Three in a row:<nl><nl>```tex<nl>\nosuchcommand{a}+\frac{1}{2}<nl>```<nl><nl>after one<nl><nl>"
          r"```tex<nl>\alsonotreal{b}\mathfrak{A}<nl>```<nl><nl>after two<nl><nl>"
          r"```tex<nl>\section*{Fine}<nl>Words here \undefinedthing<nl>```<nl><nl>after three")


def counts(d):
    ts = d.state()["typeset"]
    return {"ready": ts["ready"], "failed": ts["failed"]}


def settle(d, frames=6):
    d.wait("settle", frames=frames, timeout_ms=15000)


def until(what, check, seconds=30.0):
    end = time.time() + seconds
    while time.time() < end:
        if check():
            return
        time.sleep(0.08)
    raise AssertionError("timed out waiting for " + what)


def line_of(d):
    return d.state().get("input", {}).get("text", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", default=None)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="orbis-t-", dir="/tmp")
    shots = a.shots or tmp
    os.makedirs(shots, exist_ok=True)
    sock = os.path.join(tmp, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--token-file", a.token_file, "--drive", sock,
                             "--data-dir", os.path.join(tmp, "data")],
                            stdout=subprocess.DEVNULL, stderr=open(os.path.join(shots, "tex.log"), "w"))
    try:
        d = Drive(sock, connect_timeout=30.0)
        d.wait("attached", timeout_ms=30000)
        d.resize(1440, 900)
        until("the fold", lambda: "principal:root" in d.targets(), 60.0)
        d.click("principal:root")
        settle(d, 10)
        failures = []
        for i, reply in enumerate(REPLIES):
            d.click("input")
            settle(d)
            d.type("reply exactly: " + reply)
            settle(d)
            d.key("Return")
            # the reply lands and the wake ends: the line is ready again
            time.sleep(3.0)
            settle(d, 12)
            d.screenshot(os.path.join(shots, "reply-%d.png" % i))
            st = d.state()
            print(i, "after reply", {k: st.get(k) for k in ("input_focused",)}, repr(st.get("input_text"))[:60])
            d.click("input")
            settle(d)
            d.type("still typing %d" % i)
            settle(d)
            st = d.state()
            print(i, "typed", {k: st.get(k) for k in ("input_focused",)}, repr(st.get("input_text"))[:60])
            if st.get("input_text") != "still typing %d" % i:
                failures.append((i, st.get("input_text")))
            d.key("Escape")
            settle(d)
        ts = d.state()["typeset"]
        print("typeset", counts(d))
        if ts["ready"] < 2 or ts["failed"] != 0:
            failures.append(("typeset", ts))
        # a whole document, and text in dollars: nothing fails
        before = ts["ready"]
        for name, reply in (("document", DOCUMENT), ("fragment", FRAGMENT), ("fence", FENCE)):
            d.click("input")
            settle(d)
            d.type("reply exactly: " + reply)
            settle(d)
            d.key("Return")
            time.sleep(4.0)
            settle(d, 12)
            until("typeset settled " + name, lambda: d.state()["typeset"]["ready"] >= before + {"document": 1, "fragment": 1, "fence": 2}[name]
                  or d.state()["typeset"]["failed"] > 0, 60.0)
            settle(d, 12)
            d.screenshot(os.path.join(shots, name + ".png"))
            print(name, counts(d))
            if name == "document":
                # the whole page in view, its count, then a click to its source and one to a pane
                d.resize(1440, 1500)
                settle(d, 12)
                d.screenshot(os.path.join(shots, "page.png"))
                page = next(t for t in d.state()["typeset"]["shown"] if t["page"])
                if page["pages"] != 2:
                    failures.append(("pages", page))
                d.click(x=(page["left"] + page["right"]) / 2, y=page["bottom"] - 60)
                settle(d, 8)
                opens = [t for t in d.targets() if t.startswith("button:math-open:")]
                sources = [t for t in d.targets() if t.startswith("button:math-source:")]
                if not opens or not sources:
                    failures.append(("page buttons", d.targets()))
                else:
                    d.click(sources[0])
                    settle(d, 8)
                    d.screenshot(os.path.join(shots, "page-source.png"))
                    d.click(x=(page["left"] + page["right"]) / 2, y=page["bottom"] - 60)
                    settle(d, 8)
                    d.click([t for t in d.targets() if t.startswith("button:math-source:")][0])
                    settle(d, 8)
                    panes = len(d.panes()["live"])
                    d.click(x=(page["left"] + page["right"]) / 2, y=page["bottom"] - 60)
                    settle(d, 8)
                    d.click([t for t in d.targets() if t.startswith("button:math-open:")][0])
                    until("page in a pane", lambda: len(d.panes()["live"]) > panes, 15.0)
                    if d.panes()["error"]:
                        failures.append(("open", d.panes()["error"]))
                    settle(d, 12)
                    d.screenshot(os.path.join(shots, "page-pane.png"))
                    d.click("tab:chat")
                    settle(d, 12)
                d.resize(1440, 900)
                settle(d, 12)
        ts = d.state()["typeset"]
        if ts["failed"] != 0 or ts["ready"] != before + 2:
            failures.append(("document and fragment", ts))
        # three failures in one Message: one short reason each, and the text between them shows
        d.click("input")
        settle(d)
        d.type("reply exactly: " + BROKEN)
        settle(d)
        d.key("Return")
        until("three reasons", lambda: d.state()["typeset"]["failed"] >= 3, 60.0)
        settle(d, 12)
        d.screenshot(os.path.join(shots, "broken.png"))
        ts = d.state()["typeset"]
        print("broken", counts(d))
        notes = sorted((t for t in ts["shown"] if t["failed"]), key=lambda t: t["top"])
        for r in notes:
            print("  ", r["reason"], "|", r["source"][:30])
            if len(r["reason"]) > 120 or "\n" in r["reason"] or "warning" in r["reason"].lower() or not r["reason"]:
                failures.append(("reason", r))
            if r["note_bottom"] > r["bottom"] or r["note_top"] < r["top"]:
                failures.append(("note leaves its paragraph", r))
        for a, b in zip(notes, notes[1:]):
            if a["bottom"] > b["top"]:
                failures.append(("failures overlap", a, b))
        # the whole log, a click from the reason
        last = notes[-1]
        d.click(x=last["left"] + 60, y=last["top"] + 4)
        settle(d, 8)
        logs = [t for t in d.targets() if t.startswith("button:math-log:")]
        if not logs:
            failures.append(("no log button", d.targets()))
        else:
            d.click(logs[0])
            until("the log", lambda: any(t["log"] > 200 for t in d.state()["typeset"]["shown"]), 15.0)
            settle(d, 8)
            d.screenshot(os.path.join(shots, "log.png"))
            d.key("Escape")
            settle(d)
        # the world goes away: the line keeps taking typing, and Return keeps what was typed
        d.click("input")
        settle(d)
        with open(os.path.join(os.environ["ORBIS_WORLD_DATA"], "world.pid")) as f:
            os.kill(int(f.read().strip()), signal.SIGTERM)
        until("the world gone", lambda: not d.state().get("attached", True), 30.0)
        settle(d)
        d.click("input")
        settle(d)
        d.type("typed while it restarts")
        settle(d)
        d.key("Return")
        settle(d)
        st = d.state()
        d.screenshot(os.path.join(shots, "offline.png"))
        print("offline", repr(st.get("input_text")), st.get("input_focused"))
        if st.get("input_text") != "typed while it restarts":
            failures.append(("offline", st.get("input_text")))
        if failures:
            print("[tex] FAIL", failures)
            d.screenshot(os.path.join(shots, "fail.png"))
            return 1
        print("[tex] ok")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
