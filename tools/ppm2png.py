#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
# SPDX-License-Identifier: Apache-2.0
"""PPM(P6) → PNG，只用标准库 zlib。

给 test_apps/host_ui_render 用：它把控件渲染成 PPM（不依赖任何图片库），而
多数看图工具（以及 agent 的 read_image）只认 PNG，所以拿这个转一下：

    tools/venv/Scripts/python.exe tools/ppm2png.py build/renders/k26.ppm k26.png

只用标准库是有意的：tools/venv 里没有 PIL，工程也禁止随手装包。
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    if not data.startswith(b'P6'):
        raise SystemExit('not a P6 ppm')
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b'#':
            while data[idx:idx + 1] != b'\n':
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    w, h, _ = vals
    return w, h, data[idx:idx + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # 每行一个 filter 字节（0 = None）
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload
                + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)   # 8 bit truecolour
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: ppm2png.py in.ppm out.png')
    src, dst = sys.argv[1], sys.argv[2]
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    print(f'{src} -> {dst} ({w}x{h})')


if __name__ == '__main__':
    main()
