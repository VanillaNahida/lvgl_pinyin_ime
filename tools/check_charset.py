#!/usr/bin/env python3
"""Check that the generated fonts can actually draw what the UI asks for.

Two separate things can go wrong, and only checking one of them hides the other:

  1. The charset (generated/charset.txt) may lack a character the UI draws.
  2. The character may be in the charset but missing from the generated font -
     lv_font_conv silently drops symbols it cannot map.

This walks the same cmap lookup LVGL performs (see get_glyph_dsc_id in
lv_font_fmt_txt.c) over the generated C font and reports anything the keyboard
would draw as a blank key or a box.

Usage:
    python tools/check_charset.py
"""

from __future__ import annotations

import pathlib
import re
import sys

# LV_FONT_FMT_TXT_CMAP_* values, in enum order.
CMAP_FORMAT0_FULL = 0
CMAP_SPARSE_FULL = 1
CMAP_FORMAT0_TINY = 2
CMAP_SPARSE_TINY = 3

# The generator writes the enum name; map it to the value above.
CMAP_NAMES = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": CMAP_FORMAT0_FULL,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": CMAP_SPARSE_FULL,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": CMAP_FORMAT0_TINY,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": CMAP_SPARSE_TINY,
}

# The LVGL symbol codepoints the widget code may reference by name.
LV_SYMBOLS = {
    "LV_SYMBOL_BACKSPACE": "\uF55A",
    "LV_SYMBOL_NEW_LINE": "\uF8A2",
    "LV_SYMBOL_KEYBOARD": "\uF444",
    "LV_SYMBOL_OK": "\uF00C",
    "LV_SYMBOL_CLOSE": "\uF00D",
    "LV_SYMBOL_LEFT": "\uF104",
    "LV_SYMBOL_RIGHT": "\uF105",
    "LV_SYMBOL_UP": "\uF077",
    "LV_SYMBOL_DOWN": "\uF078",
    "LV_SYMBOL_SETTINGS": "\uF013",
}

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
INT = re.compile(r"0x[0-9A-Fa-f]+|\b\d+\b")


class Font:
    """The cmap tables of one generated font, parsed from its C source."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        text = path.read_text(encoding="utf-8")

        # Named uint16 arrays (sparse maps) and uint8 arrays (full maps).
        self.lists: dict[str, list[int]] = {}
        for m in re.finditer(r"static const uint(?:8|16)_t (\w+)\[\] = \{(.*?)\};", text, re.S):
            self.lists[m.group(1)] = [int(v, 0) for v in INT.findall(m.group(2))]

        m = re.search(r"cmaps\[\] =\s*\{(.*?)\n\};", text, re.S)
        if m is None:
            raise SystemExit(f"{path.name}: no cmaps[] table")
        self.cmaps = []
        for entry in re.finditer(r"\{([^{}]*)\}", m.group(1)):
            body = entry.group(1)
            # CMAP type is written either as a number or as the enum name.
            type_text = re.search(r"type = (\w+)", body).group(1)
            self.cmaps.append({
                "range_start": int(re.search(r"range_start = (\d+)", body).group(1)),
                "range_length": int(re.search(r"range_length = (\d+)", body).group(1)),
                "glyph_id_start": int(re.search(r"glyph_id_start = (\d+)", body).group(1)),
                "unicode_list": re.search(r"unicode_list = (\w+)", body).group(1),
                "glyph_id_ofs_list": re.search(r"glyph_id_ofs_list = (\w+)", body).group(1),
                "type": CMAP_NAMES[type_text] if type_text in CMAP_NAMES else int(type_text),
            })

        self.glyph_count = len(re.findall(r"\.bitmap_index = ", text))

    def _search(self, values: list[int], key: int) -> int | None:
        """lv_utils_bsearch: index of key in values, or None."""
        lo, hi = 0, len(values) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if values[mid] == key:
                return mid
            if values[mid] < key:
                lo = mid + 1
            else:
                hi = mid - 1
        return None

    def has(self, codepoint: int) -> bool:
        for cmap in self.cmaps:
            rcp = codepoint - cmap["range_start"]
            if rcp < 0 or rcp >= cmap["range_length"]:
                continue
            kind = cmap["type"]
            if kind == CMAP_FORMAT0_TINY:
                return True
            if kind == CMAP_FORMAT0_FULL:
                # get_glyph_dsc_id(): glyph_id_ofs_list[rcp], an offset of 0 means
                # "missing" unless this is the first character of the range.
                ofs = self.lists.get(cmap["glyph_id_ofs_list"])
                if ofs is None or rcp >= len(ofs):
                    return False
                return ofs[rcp] != 0 or codepoint == cmap["range_start"]
            if kind in (CMAP_SPARSE_TINY, CMAP_SPARSE_FULL):
                ulist = self.lists.get(cmap["unicode_list"])
                if ulist is None:
                    return False
                return self._search(ulist, rcp) is not None
            return False
        return False


def ui_characters(root: pathlib.Path) -> dict[str, set[str]]:
    """Every non-ASCII character the UI sources can draw."""
    wanted: dict[str, set[str]] = {}
    sources = sorted((root / "src" / "ui").glob("*.c")) + sorted((root / "src" / "ui").glob("*.h"))
    for path in sources:
        text = path.read_text(encoding="utf-8")
        for literal in LITERAL.findall(text):
            resolved = literal
            for name, char in LV_SYMBOLS.items():
                resolved = resolved.replace(name, char)
            for char in resolved:
                if ord(char) > 0x7F:
                    wanted.setdefault(char, set()).add(path.name)
    return wanted


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    charset_file = root / "generated" / "charset.txt"
    if not charset_file.exists():
        sys.exit(f"{charset_file} not found; run tools/gen_charset.py")
    charset = set(charset_file.read_text(encoding="utf-8").split())

    fonts = {}
    for size in (20, 16):
        path = root / "generated" / f"lv_font_ime_{size}.c"
        if not path.exists():
            sys.exit(f"{path} not found; run tools/gen_font.py")
        fonts[size] = Font(path)

    print(f"charset: {len(charset)} glyphs; "
          + ", ".join(f"{size}px: {len(fonts[size].cmaps)} cmaps" for size in fonts))

    wanted = ui_characters(root)
    print(f"UI draws {len(wanted)} distinct non-ASCII characters")

    problems = 0

    missing_charset = {c: w for c, w in wanted.items() if c not in charset}
    if missing_charset:
        problems += len(missing_charset)
        print("\nNOT IN CHARSET (add to tools/gen_charset.py):")
        for char, where in sorted(missing_charset.items()):
            print(f"  U+{ord(char):04X} {char!r} <- {', '.join(sorted(where))}")

    for size, font in fonts.items():
        missing_font = {c: w for c, w in wanted.items() if not font.has(ord(c))}
        if missing_font:
            problems += len(missing_font)
            print(f"\nMISSING FROM THE {size}px FONT (in the charset but the font has no glyph):")
            for char, where in sorted(missing_font.items()):
                tag = "" if char not in charset else " (in charset - lv_font_conv dropped it)"
                print(f"  U+{ord(char):04X} {char!r}{tag} <- {', '.join(sorted(where))}")

    if problems:
        print(f"\nresult: {problems} problem(s); those characters render as blank keys or boxes")
        return 1

    print("\nresult: every character the UI draws exists in both generated fonts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
