#!/usr/bin/env python3
"""Build the font character set for the IME.

The set is the union of every character the engine can actually put on screen:

1. GB2312 (6763 Hanzi plus punctuation and Latin), enumerated with the standard
   library codec, so the result does not depend on any extra package.
2. The dictionary's own character sets, both taken from data/dict/:
     valid_utf16.txt                what the engine may output (16 465 Hanzi)
     rawdict_utf16_65105_freq.txt   what the word list mentions (16 462 Hanzi)
   Without these the candidate bar draws the missing glyph box for the ~9.7k
   characters the dictionary contains but GB2312 does not.
3. data/dict/extra_chars.txt, hand written additions (symbols, punctuation,
   rare characters that no dictionary entry uses).

Outputs (under generated/):
    charset.txt               one character per line, ready for lv_font_conv
    charset_gb2312.txt        GB2312 only, for the smallest possible font
    charset_full.txt          the full union
    font_coverage_report.txt  what went in and what is still missing

Usage:
    python tools/gen_charset.py                        # full dictionary coverage
    python tools/gen_charset.py --gb2312-only          # smallest font
    python tools/gen_charset.py --extra-scan <word list>
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
    """Every character that occurs in a UTF-16LE word list or valid set.

    This used to keep only BMP Hanzi (0x4E00..0x9FFF), which silently dropped
    what the word lists also contain: the warning sign in a few entries, Latin
    letters, digits and the full width punctuation some words carry. Anything
    the dictionary can put on screen has to have a glyph, so collect it all and
    let the coverage report say what is still missing.
    """
    raw = path.read_bytes()
    text = raw.decode("utf-16-le", errors="replace")
    if text.startswith("\ufeff"):
        text = text[1:]

    out: set[str] = set()
    for ch in text:
        # Whitespace and control characters are not glyphs the font needs;
        # U+0020 is added separately (see gen_font.py's --range 32).
        if ch.isspace() or ord(ch) < 0x20:
            continue
        out.add(ch)
    return out


def scan_extra_chars(path: pathlib.Path) -> set[str]:
    """Every non-blank character in extra_chars.txt.

    The file is free form on purpose: whole lines starting with '#' are
    comments, whitespace is ignored (those glyphs always exist), and everything
    else is collected and de-duplicated.
    """
    out: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            continue
        for ch in line:
            if not ch.isspace():
                out.add(ch)
    return out


def write_lines(path: pathlib.Path, chars: list[str]) -> None:
    # newline="\n" keeps the file byte-identical on Windows and POSIX: with the
    # platform default, Windows writes CRLF and the sha256 of a regenerated
    # charset differs from the one produced on Linux for no reason.
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("".join(ch + "\n" for ch in chars))


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--extra-scan", default=None,
                    help="word list to scan for Hanzi (default: "
                         "data/dict/rawdict_utf16_65105_freq.txt)")
    ap.add_argument("--valid-scan", default=None,
                    help="character set the engine can actually output (default: "
                         "data/dict/valid_utf16.txt); the coverage report is about this set")
    ap.add_argument("--extra-chars", default=None,
                    help="hand written additions (default: data/dict/extra_chars.txt)")
    ap.add_argument("--gb2312-only", action="store_true",
                    help="restrict the font to GB2312 + UI + ASCII. Much smaller, "
                         "but every dictionary character outside GB2312 then renders "
                         "as the missing glyph box.")
    ap.add_argument("--with-extra-hanzi", action="store_true",
                    help="deprecated no-op: including the dictionary characters is "
                         "the default now, use --gb2312-only to opt out")
    ap.add_argument("--no-extra-scan", action="store_true",
                    help="deprecated; same as --gb2312-only for the word list")
    ap.add_argument("--out-dir", default=str(root / "generated"))
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    gb = gb2312_chars()

    scan_path = pathlib.Path(args.extra_scan) if args.extra_scan else \
        root / "data" / "dict" / "rawdict_utf16_65105_freq.txt"
    scanned: set[str] = set()
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

    extra_path = pathlib.Path(args.extra_chars) if args.extra_chars else \
        root / "data" / "dict" / "extra_chars.txt"
    extra: set[str] = set()
    if extra_path.exists():
        extra = scan_extra_chars(extra_path)
    else:
        print(f"note: extra character file not found, skipping: {extra_path}")

    ui = set(EXTRA_UI)
    dictionary_chars = scanned | engine_chars
    # Whitespace characters (U+3000 from GB2312, a stray NBSP in a word list)
    # never need a glyph, and gen_font.py reads this file with str.split(), which
    # drops them anyway - keeping them here would only make the reported glyph
    # count disagree with the font that is actually built. U+0020 is supplied to
    # lv_font_conv separately (--range 32).
    full = sorted(c for c in set(gb) | ui | dictionary_chars | extra | set(ASCII)
                  if not c.isspace())

    if args.gb2312_only or args.no_extra_scan:
        merged = sorted(set(gb) | ui | set(ASCII))
        why = "GB2312 + UI + ASCII"
    else:
        merged = full
        why = "GB2312 + UI + ASCII + dictionary + extra_chars.txt"

    # Characters the engine can output but this font would miss.
    missing = sorted(engine_chars - set(merged))

    write_lines(out_dir / "charset_gb2312.txt", gb)
    write_lines(out_dir / "charset_full.txt", full)
    write_lines(out_dir / "charset.txt", merged)

    report = [
        "lvgl_pinyin_ime font coverage report",
        "===================================",
        f"GB2312 characters          : {len(gb)}",
        f"UI symbols beyond GB2312   : {len(ui - set(gb))}",
        f"ASCII added                : {len(ASCII)}",
        f"word list scanned          : {scan_path if scanned else '(none)'}",
        f"characters in the word list: {len(scanned)}",
        f"engine output set          : {valid_path if engine_chars else '(none)'}"
        f"  ({len(engine_chars)} characters)",
        f"extra_chars.txt            : {extra_path if extra else '(none)'}"
        f"  ({len(extra)} characters)",
        "",
        f"font character set         : {len(merged)}  ({why})",
        "",
        f"engine characters this font lacks: {len(missing)}",
    ]
    if missing:
        preview = missing[:80]
        report.append("".join(preview)
                      + (f"  ... and {len(missing) - len(preview)} more"
                         if len(missing) > len(preview) else ""))
        report.append("")
        if args.gb2312_only or args.no_extra_scan:
            report.append("These render as the missing glyph box. Drop --gb2312-only to")
            report.append("include every dictionary character in the font.")
        else:
            report.append("These are missing from GB2312 AND from the dictionary; the engine")
            report.append("can only reach them through a word list entry. Add the ones you")
            report.append("need to data/dict/extra_chars.txt.")
    else:
        report.append("(none - every character the engine can output has a glyph)")

    (out_dir / "font_coverage_report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"GB2312 {len(gb)} + UI {len(ui - set(gb))} + ASCII {len(ASCII)} "
          f"+ word list {len(scanned)} + engine {len(engine_chars)} + extra {len(extra)}")
    print(f"font charset {len(merged)} characters -> {out_dir / 'charset.txt'}")
    print(f"engine output set: {len(engine_chars)} characters, {len(missing)} missing from the font")
    print(f"report -> {out_dir / 'font_coverage_report.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
