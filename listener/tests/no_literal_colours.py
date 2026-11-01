#!/usr/bin/env python3
"""Panes use colour roles only. A literal colour anywhere but the theme code fails this test."""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent / "src"
allowed = {"ui/Theme.cpp", "ui/Theme.hpp", "ui/Chrome.cpp", "ui/ThemeSpec.cpp", "ui/ThemeSpec.hpp"}
literal = re.compile(r"IM_COL32\(|ImColor\(|ImVec4\(\s*[\d.]|Palette::|0x[0-9A-Fa-f]{8}\b")
failed = []
for folder in ("ui", "app", "modules", "platform"):
    for path in sorted((root / folder).rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.suffix not in (".cpp", ".hpp") or rel in allowed:
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if literal.search(line.split("//")[0]):
                failed.append(f"{rel}:{number}: {line.strip()}")
print("\n".join(failed) or "no literal colours in panes")
sys.exit(1 if failed else 0)
