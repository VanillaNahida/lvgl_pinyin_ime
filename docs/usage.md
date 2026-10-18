# 使用指南

从零把这套东西跑起来：环境准备 → 生成词库与字体 → 接线 → 编译烧录 → 触摸校准 →
接进自己的工程。

只想看最短路径的话，[README 的快速开始](../README_zh-cn.md#快速开始跑示例) 已经列了四条命令；
本文补上硬件、校准与故障排查的细节。

---

## 1. 这个项目是什么

一个面向 **ESP-IDF 6.1 + LVGL 9.5** 的中文拼音输入法组件 `lvgl_pinyin_ime`，
可以作为组件被别的工程引用，也可以直接烧 `examples/k26_k9_demo` 看效果。

- 目标芯片：ESP32-S3（主要）、ESP32-P4；ESP32-S31 为 preview 目标
- 键盘：26 键全键盘、九键（T9）、数字/符号面板
- 输入：全拼、词组、整句；候选翻页；音节分隔符 `'`；中英切换
- 能力与「尚未落地」的功能清单见 [README](../README_zh-cn.md#功能与现状)

---

## 2. 环境准备

| 需要什么 | 说明 |
| --- | --- |
| ESP-IDF 6.1 | 例如本机路径 `D:\Espressif\EIM\v6.1\esp-idf`，换成你自己的 |
| LVGL 9.5 | 不用手动装，首次构建时组件管理器会自动拉到 `managed_components/`（需联网） |
| Node.js LTS | 只有生成字体时才用得到（`lv_font_conv` 依赖） |
| Python 3.8+ | 生成词库与字体；一律用 `tools/venv`，不要装到全局 |

### 2.1 每个新终端都要先 export

`idf.py`、`esptool` 这些命令都由 export 脚本注入当前会话，不 export 就会提示找不到命令：

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"    # 换成你的 IDF 路径
```

> 若该 IDF 的 Python 环境尚未创建，先跑一次 `install.ps1`。安装器末尾可能提示
> `Invalid feature specifier: -IdfTarget`，该报错无害，Python 环境已经建好。

---

## 3. 生成词库与字体（首次必须）

仓库的生成物不入库（`generated/` 与 `data/dict/dict_pinyin.dat` 都在 `.gitignore` 里），
所以首次检出后必须先生成，否则构建会因缺少 `generated/lv_font_ime_*.c` 直接报错。

```powershell
cd <仓库根>

# 建独立 venv 并装依赖（只需一次）
powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1

# 字表 -> 字体 C 数组；词表 -> 词库
.\tools\venv\Scripts\python.exe tools\gen_charset.py
.\tools\venv\Scripts\python.exe tools\gen_font.py
python tools\gen_dict.py
```

词库不下载官方 `dict_pinyin.dat`：仓库内置上游原始词表，由仓库内的 dictbuilder
现场生成，结果可复现（实测 1 068 442 B，逐字节可复现）。词表格式、怎么加自己的词、
生成失败怎么查，见 [dict_build.md](./dict_build.md)；字体生成需要 Node.js，字数与体积的
取舍见 [integration.md](./integration.md) 第 5 节。

---

## 4. 硬件接线

参考工程 `ESP32_MusicPlayer_V4` 的板子，屏幕与触摸共用一条 SPI2 总线，各自用不同片选。

### 4.1 ST7789 屏幕（320 x 240，横向）

| 屏幕信号 | ESP32 引脚 | 备注 |
| --- | --- | --- |
| SCLK | GPIO12 | SPI2 时钟 |
| MOSI / SDA | GPIO11 | SPI2 数据 |
| MISO | GPIO13 | 屏幕不用，给触摸复用 |
| CS | GPIO10 | 屏幕片选 |
| DC / RS | GPIO9 | 数据/命令 |
| RST | GPIO14 | 复位 |
| BL | GPIO4 | 背光，低电平点亮 |
| VCC / GND | 3.3V / GND | |

### 4.2 XPT2046 触摸

| 触摸信号 | ESP32 引脚 | 备注 |
| --- | --- | --- |
| CS | GPIO15 | 触摸片选 |
| IRQ | GPIO40 | 本项目不接，走轮询 |
| 其余信号 | 与屏幕共用 SCLK / MOSI / MISO | |

屏幕侧参数已按参考工程配好：SPI 27 MHz、RGB565、BGR、不反色；触摸侧 2 MHz、轮询。
换板子时改 `examples/k26_k9_demo/main/` 里的引脚定义即可。

---

## 5. 编译、烧录、看日志

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd <仓库根>\examples\k26_k9_demo

idf.py set-target esp32s3        # 只需做一次；换芯片时再改
idf.py build
idf.py -p COM5 flash monitor     # COM5 换成你的实际串口
```

- 第一次 `set-target` / `build` 需要联网（拉 LVGL 9.5），会慢一些
- `Ctrl + ]` 退出 monitor
- 换芯片：`idf.py set-target esp32p4`；S31 属于 preview，必须写
  `idf.py --preview set-target esp32s31`，否则会被直接拒绝
- 改过 `sdkconfig` 后行为不符合预期时：删掉整个 `build/` 再构建（见第 11 节）

> **换板子**：上面的接线、校准与日志都是 `k26_k9_demo`（ST7789 + XPT2046）的。
> 板子是 **ESP32-S31-Korvo-1**（800 × 480 RGB 屏 + GT1151 电容触摸，不需要校准）时，
> 直接看 [examples/esp32s31-korvo-demo](../examples/esp32s31-korvo-demo/README.md)：
> 那边有独立的引脚表、编译命令（`idf.py --preview set-target esp32s31`）和排错表。

正常启动后串口会打印类似：

```
I (xxx) xpt2046: XPT2046 attached to SPI2_HOST (id 1), CS=15, 2000000 Hz
I (xxx) xpt2046: controller answers, first bytes: C1 18 91 00      <- 自检
I (xxx) xpt2046: calibration loaded from NVS: x=... y=...          <- 有存档时才打印
I (xxx) k26_k9_demo: lvgl_pinyin_ime demo started, 320x240 ST7789 + XPT2046, mode=0 lang=0
I (xxx) k26_k9_demo: commands: c = calibrate, x = abort, t = show calibration,
```

屏幕上方还有一行 `tap N at (x, y)`，每点一次屏幕就会变一次 —— 这是判断触摸通不通
最直接的办法：

| 现象 | 结论 |
| --- | --- |
| 点屏幕数字不涨 | 触摸链路有问题（看启动自检和 6.3 节） |
| 数字涨但坐标很离谱 | 触摸是通的，只是标定不对，敲 `c` 校准 |
| 点上半屏涨、下半屏不涨 | 有控件把点击吃掉了 |

若自检打印 `all 19 bytes are 0x00/0xFF: the controller is not driving MISO`，
说明控制器没有驱动 MISO，属接线或供电问题（CS=GPIO15、MISO=GPIO13、3.3V、GND）。

---

## 6. 触摸校准（第一次上板基本都要做一次）

触摸走的是**原始值线性映射**，出厂默认值是从参考工程那套实测标定换算来的，换一块屏
或换一个安装方向就不可能准，所以上板第一件事是校准。

校准入口刻意放在**串口**而不是屏幕按钮上：标定离谱的时候，屏幕上的按钮恰恰点不中，
"用错的标定去校准标定"会变成死锁。校准采样读的是**原始值**，不经过当前标定，
所以无论旧标定多离谱都能校。

### 6.1 串口命令

用 `idf.py -p COM5 monitor` 打开串口后，直接敲单个字符即可（不用回车）：

| 命令 | 作用 |
| --- | --- |
| `c` | 启动校准，屏幕上依次出现 4 个十字标记 |
| `x` | 取消正在进行的校准 |
| `t` | 打印当前标定，以及一次实测的原始值（按住屏幕再敲） |
| `r` | 开关原始值连续输出，用来诊断"触摸到底有没有响应" |
| `d` | 转储一次 SPI 事务的 19 个原始字节，用于排查字节序/对齐 |
| `n` | 删除 NVS 里存的标定，回到出厂默认值 |
| `h` / `?` | 帮助 |

### 6.2 校准流程

1. 敲 `c`。屏幕出现半透明遮罩 + 中间提示框，左上角出现红色十字
2. 手指按住十字中心约 0.2 秒再松开，串口打印 `point 1: raw=(...) from N samples`
3. 依次完成左上 → 右上 → 左下 → 右下四点
4. 四点采完自动换算、立即生效，并写入 NVS，屏幕显示 `Calibration stored`
5. 重启自动从 NVS 读回，不用重复校准

校准数据存在 NVS 的 `touch` 命名空间（键 `xl`/`xr`/`yt`/`yb`，外加一个 `ver` 版本号）。
**版本号的作用**：标定值和读取路径是绑定的。一旦读取路径变了（比如修了 SPI 字节序），
旧标定就是错的 —— 它会让每次点击都落到错误位置，看起来就像"触摸完全没反应"。
所以 `ver` 对不上时驱动会忽略存档、回退到出厂默认值，并打印：

```
W (xxx) xpt2046: stored calibration is from an older build (version 0, want 2), ignoring it
```

看到这行就是自动降级成功了，重新敲 `c` 校准一次即可覆盖旧数据。想手动清掉存档就敲 `n`。

### 6.3 校准失败 / 触摸不响应怎么办

| 串口现象 | 含义 | 处理 |
| --- | --- | --- |
| `the two axes look exchanged` | 横竖两轴对调了，采样跨度异常 | 把 [touch_xpt2046.c](../examples/k26_k9_demo/main/touch_xpt2046.c) 顶部 `TOUCH_SWAP_AXES` 改成 `1`，重新编译再校准 |
| `calibration failed` | 采样值跨度太小 / 不合理 | 重新敲 `c`，手指按准十字中心、按住久一点 |
| `stored calibration is from an older build ... ignoring it` | NVS 里的标定是旧版本代码写的，已自动忽略 | 正常现象，重新敲 `c` 校准覆盖即可 |
| 启动时 `all 19 bytes are 0x00/0xFF` | 控制器没有驱动 MISO | 接线/供电问题：CS=GPIO15、MISO=GPIO13、3.3V、GND |
| 敲 `r` 后每 250 ms 一行 `raw=(...) z=...` | 控制器在响应 | 问题只是标定，按 6.2 校准即可 |
| 敲 `r` 后按住屏幕 `z` 恒为 0 | 读到的是无效数据 | 查接线；确认启动自检那两行 |
| 敲 `r` 后一行都不打印 | 驱动没起来 | 看启动日志里有没有 `XPT2046 attached to SPI2_HOST` |

**没摸屏幕时 `raw` 是噪声，随机跳是正常的** —— 电阻屏没被按下时 ADC 读数本来就没有
意义，判断"有没有触摸"要看 `z`：`z > 0` 且稳定说明控制器正常；`z` 恒为 0 表示这一帧
没有触摸（不是故障）；按住屏幕后 `z` 仍恒为 0 才是真的有问题。

按住屏幕时，`raw` 应在 200~3900 之间随手指位置单调变化。**任何 `raw` 超过 4095 都是
不可能的**（XPT2046 是 12 位），超过就说明 SPI 读出的字节拼装对齐错了 —— 用 `d` 命令
把 19 个字节打出来定位，判据是「哪个拼装方式的高字节最高位恒为 0」，那是转换的 busy 位。

---

## 7. 显示不对怎么办

| 现象 | 原因 | 怎么改 |
| --- | --- | --- |
| 全黑 | 背光没亮 / 没复位 | 检查 GPIO4 电平（低电平点亮）、RST 接线 |
| 颜色发灰、发虚、像负片 | ST7789 模块的反色要求不同 | [main.c](../examples/k26_k9_demo/main/main.c) 里把 `esp_lcd_panel_invert_color(panel, false)` 改成 `true` |
| 红蓝互换（绿变橙、红变蓝） | 面板色彩顺序是 RGB 而不是 BGR | 把 `panel_cfg.rgb_ele_order` 从 `LCD_RGB_ELEMENT_ORDER_BGR` 改成 `LCD_RGB_ELEMENT_ORDER_RGB` |
| 画面镜像 / 上下颠倒 | 面板安装方向不同 | 改 `esp_lcd_panel_mirror(panel, true, false)` 的两个参数 |
| 画面是竖的（240x320） | 没交换轴 | 确认 `esp_lcd_panel_swap_xy(panel, true)` 在调用 |
| 边缘有偏移/彩线 | 面板有 gap | `esp_lcd_panel_set_gap(panel, x, y)` |
| 文字边缘有彩色毛刺、整体发虚 | 颜色字节序不对 | 确认 `LV_COLOR_FORMAT_RGB565_SWAPPED` 在生效（见下） |

### 7.1 颜色字节序

ST7789 是**大端**器件，要求每个 RGB565 像素先发高字节；LVGL 内部是**小端**存储。
两边直接对接会把字节顺序搞反，表现为颜色错乱、抗锯齿文字边缘串色。本项目在
[main.c](../examples/k26_k9_demo/main/main.c) 里用

```c
lv_display_set_color_format(s_lv_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
```

让 LVGL 直接按大端渲染，颜色与文字锐度都恢复正常，且不额外消耗 CPU。
自己接屏幕时若漏了这一句，就会看到上面那些症状。

---

## 8. 在自己的工程里引用这个组件

完整说明（依赖声明、分区表、词库来源、字体两种模式）见
[integration.md](./integration.md)。这里给最小可用版本：

`main/idf_component.yml`：

```yaml
dependencies:
  idf: ">=5.3"
  lvgl/lvgl: "^9.5"
  lvgl_pinyin_ime:
    git: https://github.com/VanillaNahida/lvgl_pinyin_ime
    # 需要固定版本时加一行（tag / 分支 / commit 都行）；不写则跟默认分支 main：
    # version: "main"
    # 开发本仓库时改用本地路径：
    # override_path: "../../lvgl_pinyin_ime"
```

> 这两行只是声明依赖。组件还依赖仓库内自带的 `libgooglepinyin`，且 `generated/`
> 不入库 —— 工程根 `CMakeLists.txt` 要把
> `managed_components/lvgl_pinyin_ime/src/third_party/libgooglepinyin` 加进
> `EXTRA_COMPONENT_DIRS`，并在拉取下来的组件目录里生成一次词库与字体。
> 见 [integration.md](./integration.md) 第 2.2 节。

`main/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES lvgl lvgl_pinyin_ime)
```

代码：

```c
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

lv_obj_t *ta  = lv_textarea_create(lv_screen_active());
lv_obj_t *ime = lv_pinyin_ime_create(lv_screen_active());

lv_pinyin_ime_attach(ime, ta);                                  /* 上屏目标 */
lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K26);            /* 26 键 / 九键 */
lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_CN);             /* 中文 / 英文 */
lv_obj_add_event_cb(ime, on_ready, lv_pinyin_ime_event_ready(), NULL);
```

### 8.1 用应用自己的字库（推荐，字更全）

组件自带的两套字体覆盖 `tools/gen_charset.py` 选出的字符集（默认 = GB2312 + UI
符号 + 整本词库的字符集，17 265 字）。键盘上的说明文字一旦超出这个集合仍是**缺字
方框** —— 而同一个字在别的页面（用应用自己的字库）显示得好好的，看着就像"输入法的
字库坏了"。

应用如果已经有 CJK 字库，直接让输入法用它（必须在任何 `lv_pinyin_ime_create()` 之前调用）：

```c
lv_pinyin_ime_set_fonts(UI_FONT_TEXT, UI_FONT_TEXT);   /* big(候选/上屏) / small(键面) */
```

两个参数都可以传 `NULL` 表示"这个尺寸继续用自带的"。用了这个之后，
`generated/lv_font_ime_*.c` 那两套就成了死重量；需要省 flash 就把字体来源切换掉。

---

## 9. 常用配置项

`idf.py menuconfig` → `LVGL Pinyin IME`。**当前真正生效**的选项：

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `LV_PINYIN_IME_DICT_SRC` | 分区 | 词库来源：flash 分区 / SD 卡 / 内嵌；选分区且分区不存在时自动回退内嵌副本 |
| `LV_PINYIN_IME_DICT_SD_PATH` | `/sdcard/dict_pinyin.dat` | `DICT_SRC` 选 SD 卡时的路径 |
| `LV_PINYIN_IME_FONT_SRC` | 内嵌 | 字体来源：编进应用（默认）/ 从 `font` 分区加载 |
| `LV_PINYIN_IME_VOICE_ENABLE` 及 `Voice input` 子项 | 关 | 语音输入，见 [voice.md](./voice.md) |

> 以下选项在 `Kconfig` 里已有条目、但目前**尚未被代码使用**（对应功能属"规划中"，
> 见 [README 的功能与现状](../README_zh-cn.md#功能与现状)）：`LV_PINYIN_IME_DEFAULT_MODE`、
> `LV_PINYIN_IME_DOUBLE_SCHEME`、`LV_PINYIN_IME_FUZZY_DEFAULT`、`LV_PINYIN_IME_ASSOC_DEFAULT`、
> `LV_PINYIN_IME_MAX_CANDIDATES`、`LV_PINYIN_IME_CAND_PAGE_SIZE`、`LV_PINYIN_IME_K9_MAX_PY_CAND`、
> `LV_PINYIN_IME_MAX_PINYIN_LEN`、`LV_PINYIN_IME_DICT_PARTITION`、`LV_PINYIN_IME_FONT_PARTITION`、
> `LV_PINYIN_IME_FONT_SIZE_BIG`、`LV_PINYIN_IME_FONT_SIZE_SMALL`、`LV_PINYIN_IME_USER_PARTITION`、
> `LV_PINYIN_IME_USER_AUTOSAVE_EVERY`、`LV_PINYIN_IME_EXT_DICT_ENABLE`、`LV_PINYIN_IME_EXT_DICT_PATH`。
> 分区名与页面大小目前取自代码常量。

完整列表见 [Kconfig](../Kconfig)。

---

## 10. 目录结构

```
include/lvgl_pinyin_ime/   对外 API（LVGL 控件 + 引擎门面 + 语音）
src/ui/                    LVGL 控件实现
src/core/                  会话状态机与引擎辅助（不依赖 LVGL，可主机端单测）
src/data/                  词库 / 字体装载
src/voice/                 讯飞协议
src/third_party/           libgooglepinyin
partitions/                分区表（16 MB / 8 MB）
docs/                      文档
examples/k26_k9_demo/      可编译可烧录的示例
tools/                     PC 侧 Python 工具链
test_apps/                 主机单测与针对目标的编译检查
```

---

## 11. 常见问题

**`idf.py` 不是内部或外部命令**
没有 export。先跑 `& "<你的 IDF 路径>\export.ps1"`，且必须和 `idf.py` 在同一个终端会话里。

**构建报错说缺少 `generated/lv_font_ime_*.c`**
首次检出还没生成字体。按第 3 节跑一遍 `gen_charset.py` + `gen_font.py`。

**首次构建卡在下载 LVGL**
组件管理器要联网拉 `lvgl/lvgl`，拉完会缓存在 `managed_components/`。断网环境需手动 vendor。

**改完 `sdkconfig` 没反应**
`sdkconfig`（生成的）里已有的值优先于 `sdkconfig.defaults`。删掉 `sdkconfig`、
`sdkconfig.old` 与整个 `build/` 再 `idf.py build`，让它按 defaults 重新生成
（这些文件都在 `.gitignore` 里）。

**`idf.py fullclean` 后行为仍不符合预期**
`fullclean` 不会删 `build/config/sdkconfig.h`。它比 `sdkconfig` 新，CMake 会认为不需要
重新生成，于是编译器继续看到旧的 `CONFIG_` 宏。判断当前真实配置最快的办法：

```powershell
Select-String -Path build\config\sdkconfig.h -Pattern 'LV_PINYIN_IME'
```

**装 Python 依赖**
禁止直接 `pip install` 到全局。用 `tools/setup_env.ps1`，它在 `tools/venv` 里装。

**进入校准时崩溃，报 `A stack overflow in task main`**
LVGL 跑在 `app_main` 里，一整屏渲染 + 建控件比默认的 3584 字节栈要得多。
`sdkconfig.defaults` 已把 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 设成 12288。

**串口敲命令没反应**
命令是**单字符、不用回车**。用 `idf.py monitor` 时确保窗口有焦点；若用别的串口工具，
注意别开流控（本项目 8N1、无流控）。

**文字看着发虚、边缘有彩色毛刺**
先确认 7.1 节的 `LV_COLOR_FORMAT_RGB565_SWAPPED` 在生效。若颜色正常但字仍偏小，
那是字体点阵尺寸的正常观感，可用 `lv_pinyin_ime_set_fonts()` 换成应用自己的字体。

**键盘上的说明文字显示成方框**
说明用到的字超出了内置字体的字符集。用 `lv_pinyin_ime_set_fonts()` 换成应用自己的
中文字体即可，见 8.1 节。

---

## 12. 文档索引

| 文档 | 内容 |
| --- | --- |
| [usage.md](./usage.md) | 本文：环境、接线、编译烧录、触摸校准、故障排查 |
| [integration.md](./integration.md) | 组件接入、分区表、词库、字体 |
| [keymap.md](./keymap.md) | 键位与布局规范（26 键 / 九键 / P1–P3 面板） |
| [architecture.md](./architecture.md) | 分层架构、模块清单、数据流、算法 |
| [voice.md](./voice.md) | 语音输入接入与实现要点 |
| [dict_format.md](./dict_format.md) | 词库 / 字体镜像的二进制格式 |
