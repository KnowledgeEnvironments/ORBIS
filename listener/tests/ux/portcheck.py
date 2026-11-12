#!/usr/bin/env python3
"""Fail closed if a test port has a listener or any live TCP connection.

Inspect both endpoints (IPv4 and IPv6), not just accepting sockets: a reconnecting
client from an earlier run can otherwise attach to a newly started test world.
"""
import argparse
import re
import shutil
import subprocess


def require_idle(ports):
    ports = sorted(set(map(int, ports)))
    lsof = shutil.which("lsof") or shutil.which("/usr/sbin/lsof")
    if not lsof:
        raise RuntimeError("test-port preflight requires lsof; cannot verify isolation")
    result = subprocess.run([lsof, "-nP", "-iTCP:" + ",".join(map(str, ports))],
                            capture_output=True, text=True)
    if result.returncode not in (0, 1) or result.stderr.strip():
        raise RuntimeError("test-port inspection failed: " + result.stderr.strip())
    if result.stdout.strip():
        raise RuntimeError("test ports already listening or connected; stop stale test processes:\n"
                           + result.stdout)


def suite_ports(source):
    text = source.read_text()
    # Private-world scripted models bind an ephemeral port, never a neighbour's world port.
    return sorted(set(map(int, re.findall(r"--(?:stub-)?port (\d+)", text))) | {7781, 7786})


if __name__ == "__main__":
    from pathlib import Path
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", type=Path, required=True)
    args = ap.parse_args()
    try:
        ports = suite_ports(args.cmake)
        require_idle(ports)
        print("test-port preflight: idle:", ", ".join(map(str, ports)))
    except RuntimeError as exc:
        ap.exit(1, str(exc) + "\n")
