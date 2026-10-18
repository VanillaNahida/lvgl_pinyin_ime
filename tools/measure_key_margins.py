"""量一张键盘截图里"按键 vs 面板"的左右留白。

用法：python tools/measure_key_margins.py build/renders/layout/2_k26_after.png
（临时诊断脚本，不是构建链的一部分）
"""

import sys
from collections import Counter

from PIL import Image

PANEL = (0xD3, 0xD8, 0xE0)   # IME_COLOR_BG，键盘底板
KEY = (0xFF, 0xFF, 0xFF)      # IME_COLOR_KEY，普通键
SPECIAL = (0xF0, 0xF2, 0xF6)  # IME_COLOR_SPECIAL，功能键


def near(px, rgb, tol=6):
    return all(abs(px[i] - rgb[i]) <= tol for i in range(3))


def main(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    # 键盘面板的包围盒（面板色出现在成片的行里才算）
    rows = []
    for y in range(h):
        xs = [x for x in range(w) if near(px[x, y], PANEL)]
        if len(xs) > w // 3:
            rows.append((y, min(xs), max(xs)))
    if not rows:
        print("没有找到键盘面板")
        return
    top, bottom = rows[0][0], rows[-1][0]
    left = min(r[1] for r in rows)
    right = max(r[2] for r in rows)
    print(f"面板: x {left}..{right} (宽 {right - left + 1}), y {top}..{bottom}")

    # 逐行扫描：每一条"键行"里，第一个和最后一个键像素到面板边缘的距离
    print(f"{'y范围':>12}  {'键左边界':>8} {'左留白':>6}  {'键右边界':>8} {'右留白':>6}  颜色样本")
    y = top
    while y <= bottom:
        keyxs = [x for x in range(left, right + 1)
                 if near(px[x, y], KEY) or near(px[x, y], SPECIAL)]
        if not keyxs:
            y += 1
            continue
        # 找这一条键行的范围（连续若干行都有键像素）
        y0 = y
        first, last = min(keyxs), max(keyxs)
        while y <= bottom:
            xs = [x for x in range(left, right + 1)
                  if near(px[x, y], KEY) or near(px[x, y], SPECIAL)]
            if not xs:
                break
            # ★ 取整条键行的**最大跨度**，而不是最后一行的跨度：键有 4 px 圆角，
            #   最上/最下一行会窄掉几个像素，只看那一行会把圆角当成留白。
            first = min(first, min(xs))
            last = max(last, max(xs))
            y += 1
        y1 = y - 1
        c = Counter(px[x, (y0 + y1) // 2] for x in range(left, right + 1))
        sample = c.most_common(2)
        print(f"{y0:>5}..{y1:<6} {first:>8} {first - left:>6}  {last:>8} {right - last:>6}  "
              f"{sample}")
        y = y1 + 1


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/renders/layout/2_k26_after.png")
