#!/usr/bin/env python3
"""Build the offline dictionary builder (dictbuilder) from the vendored
libgooglepinyin sources, then turn the raw word list into dict_pinyin.dat.

This replaces the "download a prebuilt dict_pinyin.dat" step from the original
plan: the repository ships the upstream raw word list, so the binary dictionary
is reproducible from source with the same code the firmware runs.

Usage:
    python tools/gen_dict.py                # build the tool if needed, then the dict
    python tools/gen_dict.py --rebuild-tool # force recompiling dictbuilder
    python tools/gen_dict.py --cc g++       # use a specific C++ compiler

Outputs:
    generated/dictbuilder[.exe]     the offline model builder
    data/dict/dict_pinyin.dat       the binary dictionary
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

# Build-model sources: the runtime sources plus the offline builder.
RUNTIME_SRCS = [
    "dictio.cpp",
    "dictlist.cpp",
    "dicttrie.cpp",
    "lpicache.cpp",
    "matrixsearch.cpp",
    "mystdlib.cpp",
    "ngram.cpp",
    "pinyinime.cpp",
    "searchutility.cpp",
    "spellingtable.cpp",
    "spellingtrie.cpp",
    "splparser.cpp",
    "sync.cpp",
    "userdict.cpp",
    "utf16char.cpp",
]
BUILDER_SRCS = ["dictbuilder.cpp", "utf16reader.cpp"]


def find_compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ("g++", "c++", "clang++"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("no C++ compiler found on PATH (pass --cc)")


def build_tool(root: pathlib.Path, cc: str, force: bool) -> pathlib.Path:
    lgp = root / "src" / "third_party" / "libgooglepinyin"
    out_dir = root / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / ("dictbuilder.exe" if os.name == "nt" else "dictbuilder")

    if exe.exists() and not force:
        print(f"dictbuilder already present: {exe}")
        return exe

    sources = [str(lgp / "src" / s) for s in RUNTIME_SRCS + BUILDER_SRCS]
    sources.append(str(lgp / "tools" / "pinyinime_dictbuilder.cpp"))

    # dictbuilder needs ___BUILD_MODEL___ (the default), so the runtime-only
    # switch must not be defined here.
    cmd = [
        cc,
        "-O2",
        "-w",
        "-std=gnu++11",
        "-I",
        str(lgp / "include"),
        "-include",
        str(lgp / "port" / "pinyinime_compat.h"),
        "-o",
        str(exe),
        *sources,
    ]
    print("building dictbuilder ...")
    subprocess.run(cmd, check=True)
    return exe


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=None, help="C++ compiler to use")
    ap.add_argument("--rebuild-tool", action="store_true")
    ap.add_argument("--raw", default=str(root / "data" / "dict" / "rawdict_utf16_65105_freq.txt"))
    ap.add_argument("--valid", default=str(root / "data" / "dict" / "valid_utf16.txt"))
    ap.add_argument("--output", default=str(root / "data" / "dict" / "dict_pinyin.dat"))
    args = ap.parse_args()

    raw = pathlib.Path(args.raw)
    valid = pathlib.Path(args.valid)
    for path in (raw, valid):
        if not path.exists():
            print(f"missing input: {path}", file=sys.stderr)
            return 1

    exe = build_tool(root, find_compiler(args.cc), args.rebuild_tool)

    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    print(f"building {out.name} from {raw.name} ...")
    subprocess.run([str(exe), str(raw), str(valid), str(out)], check=True)

    size = out.stat().st_size
    print(f"wrote {out} ({size} bytes)")
    if size < 500_000:
        print("warning: the dictionary looks too small; check the raw word list", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
