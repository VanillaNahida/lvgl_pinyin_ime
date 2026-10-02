#!/usr/bin/env python3
"""Build the font character set for the IME.

Two sources are merged:

1. GB2312 (6763 Hanzi plus punctuation and Latin), enumerated with the standard
   library codec, so the result does not depend on any extra package.
2. Optionally, every BMP Hanzi that actually occurs in the dictionary word list
   (`--extra-scan`). Anything the dictionary uses but GB2312 does not contain is
   reported, because those characters would otherwise render as the missing
   glyph box.

Outputs (under generated/):
    charset.txt             one character per line, ready for lv_font_conv
    charset_gb2312.txt      GB2312 only
    font_coverage_report.txt  what the scan found and what is missing

Usage:
    python tools/gen_charset.py
    python tools/gen_charset.py --extra-scan data/dict/rawdict_utf16_65105_freq.txt
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# ASCII is included explicitly: lv_font_conv gets the Latin letters, digits and
# the punctuation the IME itself draws (the key captions).
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))

# Characters the keyboard draws that are not in GB2312: geometric shapes used
# as icon substitutes and the full width punctuation the Chinese mode emits.
#
# Everything here must exist in the source TTF as well: lv_font_conv silently
# drops a symbol the font has no glyph for, and the key then draws blank.
# DreamHanSansSC-W17 has no U+232B (⌫) and no U+2328 (⌨), which is why the
# backspace and keyboard-switch captions are ← and "abc" instead - see
# tools/check_charset.py, which verifies every caption against the built font.
EXTRA_UI = "⏎⇧✓←→↑↓‹›…—、。，；：？！《》〈〉【】〔〕±×÷°′″Ωμ§¥￥€£$≈≠≤≥⇒⇔∞√"


def gb2312_chars() -> list[str]:
    """Every character the GB2312 codec can encode, in code point order."""
    out: set[str] = set()
    for high in range(0xA1, 0xFA):
        for low in range(0xA1, 0xFF):
            try:
                ch = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.add(ch)
    return sorted(out)


def scan_dictionary(path: pathlib.Path) -> set[str]:
    """All BMP Hanzi that occur in a UTF-16LE file (word list or valid set)."""
    raw = path.read_bytes()
    text = raw.decode("utf-16-le", errors="replace")
    if text.startswith("\ufeff"):
        text = text[1:]

    out: set[str] = set()
    for ch in text:
        if 0x4E00 <= ord(ch) <= 0x9FFF:
            out.add(ch)
    return out


def write_lines(path: pathlib.Path, chars: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(ch + "\n" for ch in chars), encoding="utf-8")


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--extra-scan", default=None,
                    help="word list to scan for Hanzi (default: "
                         "data/dict/rawdict_utf16_65105_freq.txt)")
    ap.add_argument("--valid-scan", default=None,
                    help="character set the engine can actually output (default: "
                         "data/dict/valid_utf16.txt); the coverage report is about this set")
    ap.add_argument("--no-extra-scan", action="store_true")
    ap.add_argument("--with-extra-hanzi", action="store_true",
                    help="also include every Hanzi the word list contains. The "
                         "engine can output about 9.7k characters beyond GB2312; "
                         "including them makes the 20px font +2.1 MB (tools/"
                         "measure_fonts.py), which does not fit the 3 MB font "
                         "partition alongside the 16px font, so this is off by "
                         "default and the extras render as the missing glyph box")
    ap.add_argument("--out-dir", default=str(root / "generated"))
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    gb = gb2312_chars()

    scanned: set[str] = set()
    scan_path = None
    if not args.no_extra_scan:
        scan_path = pathlib.Path(args.extra_scan) if args.extra_scan else \
            root / "data" / "dict" / "rawdict_utf16_65105_freq.txt"
        if scan_path.exists():
            scanned = scan_dictionary(scan_path)
        else:
            print(f"note: scan source not found, skipping: {scan_path}")

    valid_path = pathlib.Path(args.valid_scan) if args.valid_scan else \
        root / "data" / "dict" / "valid_utf16.txt"
    engine_chars: set[str] = set()
    if valid_path.exists():
        engine_chars = scan_dictionary(valid_path)
    else:
        print(f"note: valid-character source not found, skipping: {valid_path}")

    ui = set(EXTRA_UI)
    if args.with_extra_hanzi:
        merged = sorted(set(gb) | ui | scanned | set(ASCII))
    else:
        merged = sorted(set(gb) | ui | set(ASCII))
    # Characters the engine can output but the font would miss.
    missing = sorted(engine_chars - set(gb) - ui)

    write_lines(out_dir / "charset_gb2312.txt", gb)
    write_lines(out_dir / "charset_full.txt", sorted(set(gb) | ui | scanned | set(ASCII)))
    write_lines(out_dir / "charset.txt", merged)

    report = [
        "lvgl_pinyin_ime font coverage report",
        "===================================",
        f"GB2312 characters          : {len(gb)}",
        f"UI symbols beyond GB2312   : {len(ui - set(gb))}",
        f"ASCII added                : {len(ASCII)}",
        f"word list scanned          : {scan_path if scanned else '(none)'}",
        f"Hanzi in the word list     : {len(scanned)}",
        f"engine output set          : {valid_path if engine_chars else '(none)'}"
        f"  ({len(engine_chars)} Hanzi)",
        "",
        f"font character set         : {len(merged)}"
        + ("  (GB2312 + UI + ASCII)" if not args.with_extra_hanzi else "  (including extras)"),
        "",
        f"engine characters beyond GB2312: {len(missing)}",
    ]
    if missing:
        preview = missing[:80]
        report.append("".join(preview) + (f"  ... and {len(missing) - len(preview)} more" if len(missing) > len(preview) else ""))
        report.append("")
        if args.with_extra_hanzi:
            report.append("These are included in charset.txt, so the generated font has them.")
        else:
            report.append("These are NOT in charset.txt: the 3 MB font partition only fits the")
            report.append("GB2312 set (tools/measure_fonts.py). They render as the missing glyph")
            report.append("box. Regenerate with --with-extra-hanzi to trade flash for coverage.")
    else:
        report.append("(none)")

    (out_dir / "font_coverage_report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"GB2312 {len(gb)} + UI {len(ui - set(gb))} + ASCII {len(ASCII)} + word list {len(scanned)}")
    print(f"font charset {len(merged)} characters -> {out_dir / 'charset.txt'}")
    print(f"engine output set: {len(engine_chars)} Hanzi, {len(missing)} beyond GB2312")
    print(f"report -> {out_dir / 'font_coverage_report.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
