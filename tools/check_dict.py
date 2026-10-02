#!/usr/bin/env python3
"""Report whether a dict_pinyin.dat was built for a 32-bit or a 64-bit reader.

libgooglepinyin serialises its dictionary with machine words, so a dictionary
built on a 64-bit host cannot be read by a 32-bit ESP32 and vice versa. Two
independent signatures make the width obvious, and neither needs a full parse:

  1. The spelling table header is [spelling_size_][spelling_num_][float][byte].
     spelling_size_ is small (8 for this dictionary) and spelling_num_ equals the
     number of spellings (413), so exactly one width makes the second word
     plausible and the score amplifier a sane float.

  2. The three table strides are visible in the file: with 4-byte words the
     dictionary is ~1 068 KB, with 8-byte words ~1 072 KB, because LmaNodeLE0 is
     16 bytes against 24.

The definitive check is the engine itself: test_apps/host_core_test loads the
file with the same code the firmware runs.

Usage:
    python tools/check_dict.py data/dict/dict_pinyin.dat
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

# The engine's own limits, from dictdef.h.
MAX_SPELLING_NUM = 482
LEMMA_ID_START = 1
LEMMA_ID_END = 500000


def probe(data: bytes, word: int) -> tuple[bool, str]:
    if word == 4:
        spelling_size, spelling_num = struct.unpack_from("<II", data, 0)
        off = 8
    else:
        spelling_size, spelling_num = struct.unpack_from("<QQ", data, 0)
        off = 16

    (amplifier,) = struct.unpack_from("<f", data, off)
    average = data[off + 4]
    detail = (f"spelling_size={spelling_size} spelling_num={spelling_num} "
              f"score_amplifier={amplifier:.4f} average_score={average}")

    if not (0 < spelling_size <= 16):
        return False, detail + "  (implausible spelling_size)"
    if not (0 < spelling_num <= MAX_SPELLING_NUM):
        return False, detail + "  (implausible spelling_num)"
    if not (-1000.0 < amplifier < 1000.0):
        return False, detail + "  (implausible score_amplifier)"
    if not (0 < average < 256):
        return False, detail + "  (implausible average_score)"

    table_bytes = spelling_size * spelling_num
    if off + 5 + table_bytes >= len(data):
        return False, detail + "  (spelling table runs past the end of the file)"

    return True, detail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default="data/dict/dict_pinyin.dat")
    args = ap.parse_args()

    data = pathlib.Path(args.path).read_bytes()
    print(f"{args.path}: {len(data)} bytes")

    good: list[int] = []
    for word in (4, 8):
        ok, detail = probe(data, word)
        print(f"  {word}-byte machine words: {'OK  ' if ok else 'no  '} {detail}")
        if ok:
            good.append(word)

    if not good:
        print("result: not a libgooglepinyin dictionary (or truncated)")
        return 1

    if len(good) == 2:
        print("result: ambiguous; the header is plausible either way, "
              "run test_apps/host_core_test to be sure")
        return 0

    word = good[0]
    bits = word * 8
    print(f"result: built for a {bits}-bit reader ({word}-byte words).")
    if word == 4:
        print("        ESP32 (32-bit) can read it as is.")
        return 0
    print("        ESP32 CANNOT read it: regenerate with tools/gen_dict.py")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
