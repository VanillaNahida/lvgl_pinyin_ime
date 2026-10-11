#!/usr/bin/env python3
"""把 assets/mic.png 烘成 LVGL 的 C 图像数组（src/ui/mic_icon.c）。

为什么烘成 C 数组而不是运行时读 PNG：
  - 组件至今只依赖 lvgl 与 libgooglepinyin（见 lv_pinyin_ime_voice.h 开头那段），
    运行时解码 PNG 要 LVGL 的 PNG 解码器 + 文件系统驱动，等于给所有接入方加依赖；
  - 图标是常量，24x24 的 ARGB8888 只有 2.3 KB flash，烘进去最省事。

用法（需要 Pillow）：
    python tools/gen_mic_icon.py                 # 生成 src/ui/mic_icon.c
    python tools/gen_mic_icon.py --size 20       # 换尺寸

生成物请一起提交；改了 assets/mic.png 就重跑一次。
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow：pip install pillow（或用仓库自带的 venv）")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_PNG = os.path.join(ROOT, "assets", "mic.png")
OUT_C = os.path.join(ROOT, "src", "ui", "mic_icon.c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=24, help="烘出的正方形边长（像素）")
    ap.add_argument("--src", default=SRC_PNG)
    ap.add_argument("--out", default=OUT_C)
    args = ap.parse_args()

    img = Image.open(args.src).convert("RGBA")
    # 键面小图标：用 LANCZOS 缩到目标尺寸，保持边缘平滑
    img = img.resize((args.size, args.size), Image.LANCZOS)
    w = h = args.size

    # LVGL 的 ARGB8888 在内存里的字节序是 **B, G, R, A**
    px = img.load()
    data = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            data += bytes((b, g, r, a))

    lines = []
    lines.append("/*")
    lines.append(" * mic_icon.c —— 语音键的图标（由 assets/mic.png 生成，**不要手改**）")
    lines.append(" *")
    lines.append(" * 重新生成：python tools/gen_mic_icon.py")
    lines.append(" * 格式：LV_COLOR_FORMAT_ARGB8888（内存字节序 B,G,R,A），%dx%d" % (w, h))
    lines.append(" */")
    lines.append("")
    lines.append('#include "mic_icon.h"')
    lines.append("")
    lines.append("static const uint8_t mic_icon_map[] = {")
    per = 12
    for i in range(0, len(data), per):
        chunk = data[i:i + per]
        lines.append("    " + " ".join("0x%02X," % c for c in chunk))
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t mic_icon = {")
    lines.append("    .header.cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("    .header.w = %d," % w)
    lines.append("    .header.h = %d," % h)
    lines.append("    .header.stride = %d," % (w * 4))
    lines.append("    .data_size = %d," % len(data))
    lines.append("    .data = mic_icon_map,")
    lines.append("};")
    lines.append("")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("已生成 %s（%dx%d，%d 字节像素数据）" % (args.out, w, h, len(data)))


if __name__ == "__main__":
    main()
