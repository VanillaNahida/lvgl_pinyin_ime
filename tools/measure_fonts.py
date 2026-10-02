#!/usr/bin/env python3
"""Measure font sizes for the charset / bpp options before committing to one.

Run this after tools/gen_charset.py to decide what fits the 3 MB "font"
partition. It only writes under generated/probe/.

Usage:
    python tools/measure_fonts.py
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN = ROOT / "generated"
PROBE = GEN / "probe"

CASES = [
    ("gb2312", GEN / "charset_gb2312.txt"),
    ("full", GEN / "charset.txt"),
]
BPP = [4, 2]


def main() -> int:
    PROBE.mkdir(parents=True, exist_ok=True)
    results: list[tuple[str, int, int, int]] = []

    for name, charset in CASES:
        if not charset.exists():
            print(f"skip {name}: {charset} missing (run tools/gen_charset.py)")
            continue
        glyphs = len(charset.read_text(encoding="utf-8").split())
        for bpp in BPP:
            cmd = [
                sys.executable,
                str(ROOT / "tools" / "gen_font.py"),
                "--charset", str(charset),
                "--bpp", str(bpp),
                "--suffix", f"_{name}_{bpp}bpp",
                "--out-dir", str(PROBE),
            ]
            print(f"--- {name}, {bpp} bpp, {glyphs} glyphs ---")
            subprocess.run(cmd, check=True)
            image = PROBE / f"font_partition_{name}_{bpp}bpp.bin"
            results.append((name, bpp, glyphs, image.stat().st_size))

    print()
    print(f"{'charset':10s} {'bpp':>3s} {'glyphs':>7s} {'partition':>12s}")
    for name, bpp, glyphs, size in results:
        print(f"{name:10s} {bpp:3d} {glyphs:7d} {size / 1024:9.0f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
