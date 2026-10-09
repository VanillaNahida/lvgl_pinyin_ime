#!/usr/bin/env python3
"""Generate the IME fonts from DreamHanSansSC-W17.ttf.

Two glyph sets are produced, one per Kconfig font size:

    lv_font_ime_<big>      candidate bar / committed text
    lv_font_ime_<small>    key captions / pinyin chip

Two output formats are supported:

  --format c    (default) a C source file per font, holding the glyph bitmaps as
                a byte array plus an `lv_font_t` named after the file. The files
                are compiled into the firmware, so there is nothing to flash and
                no file system involved. This is what makes the keyboard work
                out of the box.

  --format bin  the LVGL binary font container ("binfont"), big + small
                concatenated into generated/font_partition.bin for the optional
                "font" partition. Requires CONFIG_LV_USE_FS_MEMFS.

lv_font_conv's --symbols is a literal list of characters, NOT a file name:
passing a path makes it render the path's own characters. The charset file is
therefore read here and passed in chunks (a 7.5k glyph set would otherwise blow
the Windows command line limit).

Usage:
    python tools/gen_font.py                       # C arrays, 20px/2bpp + 16px/2bpp
    python tools/gen_font.py --format bin          # partition image
    python tools/gen_font.py --big-bpp 4 --charset generated/charset_small.txt
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import struct
import subprocess
import sys

DEFAULT_BIG = 20
DEFAULT_SMALL = 16

# lv_font_conv --symbols is a character list; split it so the command line stays
# well under the OS limit.
SYMBOLS_CHUNK = 1500


def find_font(root: pathlib.Path) -> pathlib.Path:
    for candidate in (
        root / "DreamHanSansSC-W17.ttf",
        root / "assets" / "DreamHanSansSC-W17.ttf",
    ):
        if candidate.exists():
            return candidate
    sys.exit("DreamHanSansSC-W17.ttf not found in the repository root")


def lv_font_conv_command(explicit: str | None) -> list[str]:
    if explicit:
        return [explicit]
    if sys.platform == "win32":
        npx = shutil.which("npx.cmd") or shutil.which("npx")
        if npx:
            return [npx, "--yes", "lv_font_conv@1.5.2"]
    found = shutil.which("lv_font_conv")
    if found:
        return [found]
    npx = shutil.which("npx")
    if not npx:
        sys.exit("lv_font_conv not found and npx is unavailable; install Node.js LTS")
    return [npx, "--yes", "lv_font_conv@1.5.2"]


def run_conv(cmd: list[str], font: pathlib.Path, size: int, bpp: int, symbols: str,
             out: pathlib.Path, fmt: str, no_compress: bool) -> None:
    args = [*cmd, "--font", str(font), "--size", str(size), "--bpp", str(bpp)]
    if fmt == "lvgl":
        # Without this the generated file includes "lvgl/lvgl.h", which only
        # resolves through a top level lvgl directory. "lvgl.h" is the public
        # include of the ESP-IDF lvgl component.
        args += ["--lv-include", "lvgl.h"]
    # U+0020 explicitly: it cannot ride along in --symbols because the charset
    # file format loses it (gen_charset.py writes one character per line, so a
    # space becomes a blank line). A font without a space glyph makes LVGL draw
    # its missing-glyph box at every space. --range is not split on whitespace.
    args += ["--range", "32"]
    for start in range(0, len(symbols), SYMBOLS_CHUNK):
        args += ["--symbols", symbols[start:start + SYMBOLS_CHUNK]]
    args += ["--format", fmt, "-o", str(out)]
    # --no-compress makes the generator emit raw bitmaps and record
    # bitmap_format = 0 (PLAIN). RLE compressed output (the default, format 1)
    # also works with LVGL 9 - COMPRESSED is 1 there - but raw is easier to
    # reason about and only about 25% larger at 2 bpp. Callers decide.
    if no_compress:
        args.append("--no-compress")

    result = subprocess.run(args, capture_output=True, text=True, shell=(sys.platform == "win32"))
    if result.returncode != 0:
        # Surface the tool's own message: a truncated "failed" is useless.
        sys.stderr.write(f"lv_font_conv ({fmt}, {size}px, {bpp}bpp) exited {result.returncode}\n")
        sys.stderr.write(f"  argv: {args[:8]} ... --symbols <{len(symbols)} chars> ...\n")
        if result.stdout:
            sys.stderr.write("  stdout: " + result.stdout.strip()[-1500:] + "\n")
        if result.stderr:
            sys.stderr.write("  stderr: " + result.stderr.strip()[-1500:] + "\n")
        raise SystemExit(1)


# LVGL stores a glyph's bitmap offset in a 20 bit field unless
# LV_FONT_FMT_TXT_LARGE is on, which caps glyph_bitmap[] at 1 MB. lv_font_conv
# only ever emits the compact 8 byte glyph descriptor (see
# lib/writers/lvgl/lv_table_glyf.js), so it is NOT compatible with
# LV_FONT_FMT_TXT_LARGE: with that option on, LVGL walks glyph_dsc[] at a 12 byte
# stride over 8 byte entries and every glyph comes out empty (text disappears
# entirely - no boxes, nothing). The option must stay off and the bitmap must fit.
MAX_GLYPH_BITMAP = 1048575  # 2^20 - 1

LITERAL = re.compile(r"0x[0-9A-Fa-f]{1,2}\b")


def check_c_font(path: pathlib.Path) -> dict:
    """Trim the tool's command echo and check the lv_font_t is defined.

    lv_font_conv writes the whole command line into the banner comment; with a
    7.5k character charset that is several hundred KB of noise in every diff.
    """
    text = path.read_text(encoding="utf-8")
    expected = f"lv_font_t {path.stem} ="
    if expected not in text and f"const lv_font_t {path.stem} =" not in text:
        raise SystemExit(f"{path.name} does not define {path.stem}; lv_font_conv changed?")

    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith("* Opts:"):
            size = line.split("--size")[1].split()[0] if "--size" in line else "?"
            bpp = line.split("--bpp")[1].split()[0] if "--bpp" in line else "?"
            lines[i] = f" * Opts: --size {size} --bpp {bpp} (charset omitted, see generated/charset.txt)"
            break
    text = "\n".join(lines) + "\n"

    # Measure the real glyph bitmap: the byte literals between the glyph_bitmap[]
    # declaration and the closing brace. lv_font_conv pads each glyph to a byte
    # boundary and writes up to 8 literals per line, so count literals rather than
    # lines. Note 0x1 is a valid literal, hence {1,2} in the pattern.
    start = text.find("glyph_bitmap[] = {")
    end = text.find("\n};", start)
    if start < 0 or end < 0:
        raise SystemExit(f"{path.name}: cannot find glyph_bitmap[]")
    bitmap_bytes = len(LITERAL.findall(text[start:end]))
    if bitmap_bytes < 1000:
        raise SystemExit(f"{path.name} looks empty ({bitmap_bytes} bitmap bytes); "
                         "check the charset")

    # The hard limit is the largest bitmap_index, not the bitmap size: LVGL stores
    # that offset in a 20 bit field (see MAX_GLYPH_BITMAP). It is the last glyph's
    # offset, so it can only be read from the descriptors.
    indexes = [int(m) for m in re.findall(r"\.bitmap_index = (\d+)", text)]
    if not indexes:
        raise SystemExit(f"{path.name}: no glyph descriptors found")
    max_index = max(indexes)

    m = re.search(r"\.bitmap_format = (\d+),", text)
    if m is None:
        raise SystemExit(f"{path.name}: no .bitmap_format in font_dsc")
    fmt_code = m.group(1)

    # ------------------------------------------------------------------
    # Make the output match the LVGL 9 struct layout.
    #
    # lv_font_conv 1.5.2 targets LVGL 8, and LVGL 9 changed what a font
    # descriptor has to say:
    #
    #   * lv_font_fmt_txt_dsc_t gained `stride` in LVGL 9. The generator does
    #     not emit it, so it sits at whatever the surrounding bytes happen to be.
    #
    # Note: LVGL 9.5 does NOT have an `are_glyphs_dynamic_loaded` field (it was
    # removed again), so it must NOT be injected here - doing so fails to compile
    # with "lv_font_fmt_txt_dsc_t has no member named 'are_glyphs_dynamic_loaded'".
    #
    # `bitmap_format`, by contrast, must be left EXACTLY as generated. The
    # generator emits RLE compressed bitmaps and writes 1 for that; LVGL 9's enum
    # is {PLAIN = 0, COMPRESSED = 1, COMPRESSED_NO_PREFILTER = 2}, so 1 already
    # means COMPRESSED and reads back correctly. Rewriting it to 0 (which looks
    # like the "right" fix because LVGL 8's enum was ordered differently) makes
    # LVGL read RLE data as raw pixels and every glyph renders as noise.
    # For the same reason the C format must be generated with --no-compress or
    # the mapping has to be handled explicitly - see run_conv().
    # ------------------------------------------------------------------

    if ".stride" not in text:
        # No line padding: lv_font_conv packs each glyph tightly, which is what
        # LVGL reads when stride is 0.
        anchor = ".bitmap_format = "
        idx = text.find(anchor)
        line_end = text.find("\n", idx)
        text = (text[:line_end + 1]
                + "    .stride = 0,\n"
                + text[line_end + 1:])

    # `static_bitmap = 1` tells LVGL the bitmaps live in flash and can be handed
    # out directly. Without it LVGL logs "Requesting static bitmap of a non-static
    # bitmap" and lv_font_get_glyph_static_bitmap() returns NULL.
    if ".static_bitmap" not in text:
        # The generator writes: `.dsc = &font_dsc           /*The custom font
        # data. ... */` - no trailing comma - so anchor on the field name.
        m2 = re.search(r"^([ \t]*)\.dsc = &font_dsc", text, re.M)
        if m2 is None:
            raise SystemExit(f"{path.name}: no .dsc field in the lv_font_t")
        indent = m2.group(1)
        text = (text[:m2.start()]
                + f"{indent}.static_bitmap = 1,\n"
                + text[m2.start():])

    path.write_text(text, encoding="utf-8")
    return {"size": path.stat().st_size, "bitmap": bitmap_bytes, "max_index": max_index,
            "bitmap_format": fmt_code}


def bin_font_tables(path: pathlib.Path) -> dict:
    """Validate the LVGL binary container: [u32 length][4 byte label] tables."""
    data = path.read_bytes()
    known = (b"head", b"cmap", b"loca", b"glyf", b"kern")
    labels: list[str] = []
    glyf = 0
    offset = 0
    while offset + 8 <= len(data):
        (length,) = struct.unpack_from("<I", data, offset)
        label = data[offset + 4:offset + 8]
        if label not in known or length <= 0:
            break
        labels.append(label.decode("ascii"))
        if label == b"glyf":
            glyf = length
        offset += length
        if len(labels) > 8:
            break

    if not labels or labels[0] != "head":
        raise SystemExit(f"{path} is not an LVGL binary font (first label {data[4:8]!r})")
    return {"size": len(data), "tables": labels, "glyf": glyf}


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--format", choices=["c", "bin"], default="c",
                    help="c: compile the fonts into the firmware (default); "
                         "bin: LVGL binary image for the font partition")
    ap.add_argument("--big", type=int, default=DEFAULT_BIG)
    ap.add_argument("--small", type=int, default=DEFAULT_SMALL)
    # 4 bpp for the 20 px font would need 1 053 966 bytes of glyph bitmap, over the
    # 1 048 575 byte limit of LVGL's 20 bit offset field (and LV_FONT_FMT_TXT_LARGE
    # cannot be used with lv_font_conv - see the check below). 2 bpp is 527 KB.
    ap.add_argument("--big-bpp", type=int, default=2, choices=[1, 2, 3, 4, 8])
    ap.add_argument("--small-bpp", type=int, default=2, choices=[1, 2, 3, 4, 8])
    ap.add_argument("--charset", default=str(root / "generated" / "charset.txt"))
    ap.add_argument("--font", default=None)
    ap.add_argument("--out-dir", default=str(root / "generated"))
    ap.add_argument("--lv-font-conv", default=None)
    ap.add_argument("--suffix", default="",
                    help="suffix for the partition image name, for size experiments")
    ap.add_argument("--compress", action="store_true",
                    help="bin format only: let lv_font_conv LZ4 compress")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    font = pathlib.Path(args.font) if args.font else find_font(root)
    charset = pathlib.Path(args.charset)
    if not charset.exists():
        sys.exit(f"charset not found: {charset} (run tools/gen_charset.py first)")

    chars = charset.read_text(encoding="utf-8").split()

    # Space is supplied through --range in run_conv(), because this file format
    # cannot carry U+0020 (see gen_charset.py's one-character-per-line output).
    symbols = "".join(chars)
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"font     : {font} ({font.stat().st_size} bytes)")
    print(f"charset  : {charset} ({len(chars)} glyphs)")
    print(f"format   : {args.format}")

    if args.dry_run:
        for size, bpp in ((args.big, args.big_bpp), (args.small, args.small_bpp)):
            est = len(chars) * size * size * bpp // 8
            print(f"  {size}px/{bpp}bpp: roughly {est / 1024:.0f} KB of bitmap data")
        return 0

    cmd = lv_font_conv_command(args.lv_font_conv)

    if args.format == "c":
        # One file per font. The lv_font_t is named after the file, so the names
        # must match what src/data/ime_font.c declares.
        worst = 0
        for size, bpp in ((args.big, args.big_bpp), (args.small, args.small_bpp)):
            out = out_dir / f"lv_font_ime_{size}.c"
            print(f"generating {out.name} ({size}px, {bpp}bpp) ...")
            # --no-compress: raw bitmaps with bitmap_format = 0. See run_conv().
            run_conv(cmd, font, size, bpp, symbols, out, "lvgl", no_compress=True)
            info = check_c_font(out)
            print(f"  {out.name}: {info['size']} bytes of C, "
                  f"{info['bitmap']} glyph bitmap bytes, "
                  f"largest bitmap_index {info['max_index']}, "
                  f"bitmap_format {info['bitmap_format']}")
            if info["max_index"] > MAX_GLYPH_BITMAP:
                sys.exit(
                    f"error: {out.name} needs bitmap_index {info['max_index']}, over the "
                    f"{MAX_GLYPH_BITMAP} limit of LVGL's 20 bit offset field.\n"
                    f"       LV_FONT_FMT_TXT_LARGE cannot be used instead: lv_font_conv only\n"
                    f"       emits the compact glyph descriptor, so LVGL would read\n"
                    f"       glyph_dsc[] at the wrong stride and draw nothing at all.\n"
                    f"       Lower --big-bpp, or shrink the charset.")
            worst = max(worst, info["max_index"])

        print(f"largest bitmap_index across both fonts: {worst} "
              f"(limit {MAX_GLYPH_BITMAP}; LV_FONT_FMT_TXT_LARGE must stay off)")
        return 0

    # --format bin: build the two fonts and concatenate them for the partition.
    big_bin = out_dir / f"lv_font_ime_{args.big}.bin"
    small_bin = out_dir / f"lv_font_ime_{args.small}.bin"
    no_compress = not args.compress

    for size, bpp, out in ((args.big, args.big_bpp, big_bin), (args.small, args.small_bpp, small_bin)):
        print(f"generating {out.name} ({size}px, {bpp}bpp) ...")
        run_conv(cmd, font, size, bpp, symbols, out, "bin", no_compress)
        info = bin_font_tables(out)
        per_glyph = info["glyf"] / max(1, len(chars))
        print(f"  {out.name}: {info['size']} bytes, tables {','.join(info['tables'])}, "
              f"glyf {info['glyf']} bytes ({per_glyph:.1f} per glyph)")
        if per_glyph < 8:
            sys.exit(f"error: {out.name} only has {per_glyph:.1f} glyph bytes per requested "
                     "character; the font was not rendered")

    partition = out_dir / f"font_partition{args.suffix}.bin"
    partition.write_bytes(big_bin.read_bytes() + small_bin.read_bytes())
    print(f"wrote {partition} ({partition.stat().st_size} bytes, "
          f"{partition.stat().st_size / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
