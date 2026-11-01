#!/usr/bin/env python3
"""An idle client sleeps: few frames drawn, little CPU, with the map at dusk or not, and while hidden.

    privateworld.py --port 7961 -- idle_world.py --listener <binary> --url {url} --token-file {token} --shots <dir>

The rule: block until something changes the picture. Exit 0 on pass, 1 on failure.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "..", "tools")]
from drive import Drive  # noqa: E402

MAX_FPS = 8.0   # a blink at 2 Hz and a few frames settling; animation would show as far more
MAX_CPU = 15.0  # percent of a core over the window


def cpu_seconds(pid):
    out = subprocess.check_output(["ps", "-o", "time=", "-p", str(pid)], text=True).strip()
    total = 0.0
    for part in out.replace("-", ":").split(":"):
        total = total * 60 + float(part)
    return total


def measure(d, pid, label, seconds=5.0):
    f0 = d.ping()["frame"] if d else 0
    c0 = cpu_seconds(pid)
    time.sleep(seconds)
    c1 = cpu_seconds(pid)
    f1 = d.ping()["frame"] if d else 0
    cpu = 100.0 * (c1 - c0) / seconds
    fps = (f1 - f0) / seconds
    print(f"[idle] {label}: {fps:.1f} fps, {cpu:.1f}% cpu", flush=True)
    return fps, cpu


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listener", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--shots", required=True)
    ap.add_argument("--report-only", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.shots, exist_ok=True)
    home = tempfile.mkdtemp(prefix="orbis-idle")
    sock = os.path.join(home, "d.sock")
    proc = subprocess.Popen([a.listener, "--url", a.url, "--format", "json", "--token-file", a.token_file,
                             "--data-dir", home, "--drive", sock],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    failures = []
    try:
        d = Drive(sock)
        end = time.time() + 30
        while time.time() < end and not d.state().get("attached"):
            time.sleep(0.2)
        d.wait("settle", frames=8, timeout_ms=15000)
        d.click("tab:graph")
        d.wait("settle", frames=8, timeout_ms=15000)
        time.sleep(1.5)
        fps, cpu = measure(d, proc.pid, "map")
        if fps > MAX_FPS:
            failures.append(f"map: {fps:.1f} fps")
        if cpu > MAX_CPU:
            failures.append(f"map: {cpu:.1f}% cpu")
        d.click("menu:view")
        d.wait("settle", frames=4, timeout_ms=5000)
        d.click("menu:twilight-map")
        d.wait("settle", frames=8, timeout_ms=15000)
        time.sleep(1.5)
        d.screenshot(os.path.join(a.shots, "twilight.png"))
        time.sleep(1.5)
        fps, cpu = measure(d, proc.pid, "twilight")
        if fps > MAX_FPS:
            failures.append(f"twilight: {fps:.1f} fps")
        if cpu > MAX_CPU:
            failures.append(f"twilight: {cpu:.1f}% cpu")
        d.call("minimize")
        time.sleep(2.0)
        _, cpu = measure(None, proc.pid, "minimized")
        if cpu > MAX_CPU:
            failures.append(f"minimized: {cpu:.1f}% cpu")
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if failures and not a.report_only:
        print("[idle] FAIL", failures)
        return 1
    print("[idle] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
