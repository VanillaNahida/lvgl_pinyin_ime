#!/usr/bin/env python3
"""Inspect an LVGL binfont container) and report its glyphs.

The container is a sequence of tables:
    [u32 length][4 byte label]  x N
with labels head, cmap, loca, glyf, kern. The table bodies are the C structs of
lv_binfont_loader.c, which this script mirrors exactly:

    typedef struct font_header_bin {          /* 36 bytes */
        uint32_t version;        uint16_t tables_count;   uint16_t font_size;
        uint16_t ascent;         int16_t  descent;        uint16_t typo_ascent;
        int16_t  typo_descent;   uint16_t typo_line_gap;  int16_t  min_y;
        int16_t  max_y;          uint16_t default_advance_width;
        uint16_t kerning_scale;  uint8_t  index_to_loc_format;
        uint8_t  glyph_id_format;uint8_t  advance_width_format;
        uint8_t  bits_per_pixel; uint8_t  xy_bits;        uint8_t  wh_bits;
        uint8_t  advance_width_bits; uint8_t compression_id;
        uint8_t  subpixels_mode; uint8_t  padding;
        int16_t  underline_position; uint16_t underline_thickness;
    } font_header_bin_t;

    typedef struct cmap_table_bin {           /* 16 bytes */
        uint32_t data_offset;    uint32_t range_start;    uint16_t range_length;
        uint16_t glyph_id_start; uint16_t data_entries_count;
        uint8_t  format_type;    uint8_t  padding;
    } cmap_table_bin_t;

A wrong-format file (lv_font_conv --format lvgl emits C, --format dump emits
text) is rejected before any parsing.

Usage:
    python tools/inspect_font.py generated/lv_font_ime_20.bin [--grep 你]
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

CMAP_FORMAT_TINY = 0      # 1 byte per glyph id
CMAP_FORMAT_SPARSE_TINY = 1
CMAP_FORMAT_FULL = 2
CMAP_FORMAT_SPARSE_FULL = 3

TABLE_LABELS = (b"head", b"cmap", b"loca", b"glyf", b"kern")


def read_tables(data: bytes) -> dict[str, tuple[int, int]]:
    """Return {label: (offset, length)} for each table in the container."""
    tables: dict[str, tuple[int, int]] = {}
    offset = 0
    while offset + 8 <= len(data):
        (length,) = struct.unpack_from("<I", data, offset)
        label = data[offset + 4:offset + 8]
        if label not in TABLE_LABELS or length <= 0:
            break
        tables[label.decode("ascii")] = (offset, length)
        offset += length
    return tables


def read_head(data: bytes, off: int) -> dict:
    """Best effort head decode.

    The glyph coverage question is answered by the cmap table, so a head layout
    mismatch must not stop the inspection: the fields are read one at a time and
    missing ones are simply omitted.
    """
    body = off + 8
    layout = [
        ("version", "<I"),
        ("tables", "<H"),
        ("font_size", "<H"),
        ("ascent", "<H"),
        ("descent", "<h"),
        ("typo_ascent", "<H"),
        ("typo_descent", "<h"),
        ("typo_line_gap", "<H"),
        ("min_y", "<h"),
        ("max_y", "<h"),
        ("default_advance_width", "<H"),
        ("kerning_scale", "<H"),
        ("index_to_loc_format", "<B"),
        ("glyph_id_format", "<B"),
        ("advance_width_format", "<B"),
        ("bits_per_pixel", "<B"),
        ("xy_bits", "<B"),
        ("wh_bits", "<B"),
        ("advance_width_bits", "<B"),
        ("compression_id", "<B"),
        ("subpixels_mode", "<B"),
        ("padding", "<B"),
        ("underline_position", "<h"),
        ("underline_thickness", "<H"),
    ]

    out: dict = {}
    pos = body
    for name, fmt in layout:
        size = struct.calcsize(fmt)
        if pos + size > len(data):
            break
        (out[name],) = struct.unpack_from(fmt, data, pos)
        pos += size
    return out
    return out


def read_cmap(data: bytes, off: int) -> tuple[list[dict], list[int]]:
    """Decode every cmap subtable and return the code points it covers."""
    body = off + 8
    (count,) = struct.unpack_from("<I", data, body)
    # cmap_table_bin_t is 16 bytes.
    entry_size = 16
    subtables: list[dict] = []
    codepoints: list[int] = []

    for i in range(count):
        base = body + 4 + i * entry_size
        data_offset, range_start, range_length, glyph_id_start, entries, fmt, _pad = \
            struct.unpack_from("<IIHHHBB", data, base)
        entry = {
            "range_start": range_start,
            "range_length": range_length,
            "glyph_id_start": glyph_id_start,
            "entries": entries,
            "format": fmt,
        }
        subtables.append(entry)

        pos = body + data_offset
        if fmt == CMAP_FORMAT_TINY:
            # Contiguous run with 1 byte offsets.
            codepoints.extend(range(range_start, range_start + range_length))
        elif fmt == CMAP_FORMAT_FULL:
            if entries:
                ids = struct.unpack_from(f"<{entries}H", data, pos)
                codepoints.extend(r for r, g in zip(range(range_start, range_start + entries), ids) if g)
            else:
                codepoints.extend(range(range_start, range_start + range_length))
        elif fmt in (CMAP_FORMAT_SPARSE_TINY, CMAP_FORMAT_SPARSE_FULL):
            if fmt == CMAP_FORMAT_SPARSE_TINY:
                oids = struct.unpack_from(f"<{entries}B", data, pos)
                opos = pos + entries
            else:
                oids = struct.unpack_from(f"<{entries}H", data, pos)
                opos = pos + entries * 2
            unicode_list = struct.unpack_from(f"<{entries}H", data, opos)
            codepoints.extend(u for u, o in zip(unicode_list, oids) if o)
        else:
            print(f"warning: unknown cmap format {fmt}", file=sys.stderr)

    return subtables, codepoints


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("--grep", default=None, help="report whether these characters are present")
    ap.add_argument("--max-list", type=int, default=40)
    args = ap.parse_args()

    path = pathlib.Path(args.font)
    data = path.read_bytes()
    tables = read_tables(data)
    if "head" not in tables:
        sys.exit(f"{path} is not an LVGL binary font (no head table); "
                 f"first label {data[4:8]!r}")

    print(f"{path.name}: {len(data)} bytes")
    for label, (off, length) in tables.items():
        print(f"  {label:5s} length {length:6d} at {off}")

    head = read_head(data, tables["head"][0])
    print(f"head: version {head['version']}, tables {head['tables']}, "
          f"size {head['font_size']}px, {head['bits_per_pixel']}bpp, "
          f"glyph_id_format {head['glyph_id_format']}, adv_bits {head['advance_width_bits']}, "
          f"compression {head['compression_id']}")
    print(f"      ascent {head['ascent']} descent {head['descent']} "
          f"line_gap {head['typo_line_gap']} kern_scale {head['kerning_scale']}")

    subtables, codepoints = read_cmap(data, tables["cmap"][0])
    for i, st in enumerate(subtables):
        print(f"cmap[{i}]: format {st['format']} range 0x{st['range_start']:04X} "
              f"len {st['range_length']} entries {st['entries']}")
    unique = sorted(set(codepoints))
    print(f"covered code points: {len(unique)}")
    if unique:
        # The console may be a legacy code page; never let a print() decide the
        # exit status of an inspection run.
        show = lambda cs: "".join(chr(c) for c in cs)
        try:
            print("  first:", show(unique[:args.max_list]))
            print("  last :", show(unique[-args.max_list:]))
        except UnicodeEncodeError:
            print("  first:", " ".join(f"U+{c:04X}" for c in unique[:args.max_list]))
            print("  last :", " ".join(f"U+{c:04X}" for c in unique[-args.max_list:]))

    if args.grep:
        missing = [ch for ch in args.grep if ord(ch) not in codepoints]
        present = [ch for ch in args.grep if ord(ch) in codepoints]
        print(f"present: {''.join(present) if present else '(none)'}")
        print(f"missing: {''.join(missing) if missing else '(none)'}")
        return 1 if missing else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
