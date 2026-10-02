# 组件接入说明

## 1. 环境

| 项 | 值 |
| --- | --- |
| ESP-IDF | 6.1.0（`D:\Espressif\EIM\v6.1\esp-idf`） |
| LVGL | 9.5（组件仓库 `lvgl/lvgl`，首次构建联网拉取到 `managed_components/`） |
| 目标芯片 | ESP32-S3-N16R8（主）、ESP32-S31（正式支持）、ESP32-P4（顺带支持） |
| 主机端 Python | `D:\Espressif\EIM\tools\idf-python\3.11.2\python.exe` |
| Node.js | LTS（`lv_font_conv` 依赖，需 `winget install OpenJS.NodeJS.LTS`） |

## 2. 作为组件引入

### 2.1 本仓库内的示例工程

`examples/k26_k9_demo/CMakeLists.txt` 通过 `EXTRA_COMPONENT_DIRS` 直接把仓库根目录
注册为一个 IDF 组件（ESP-IDF 的 `__project_component_dir()` 支持"目录自身即为组件"）：

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(k26_k9_demo)
```

### 2.2 独立工程引入

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=5.3"
  lvgl/lvgl: "^9.5"
  lvgl_pinyin_ime:
    version: "*"
```

或使用本地路径覆盖（开发期推荐）：

```yaml
dependencies:
  lvgl_pinyin_ime:
    override_path: "../../lvgl_pinyin_ime"
```

然后 `idf.py reconfigure` 会拉取/链接组件。

## 3. 分区表

`partitions/ime_16mb.csv`（ESP32-S3-N16R8，16 MB）：

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
dict,     data, 0x40,    0x10000,  0x180000,
font,     data, 0x41,    ,         0x300000,
usr,      data, fat,     ,         0x100000,
factory,  app,  factory, ,         0x800000,
```

合计约 13.6 MB < 16 MB。

`partitions/ime_8mb.csv`：4 MB app、无独立 font 分区、字体内嵌为 C 数组。

工程侧配置：

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/ime_16mb.csv"
```

（本仓库示例工程的 `sdkconfig.defaults` 中该值为 `"../../partitions/ime_16mb.csv"`。）

`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 相对于工程目录，示例工程因此直接写
`"../../partitions/ime_16mb.csv"` 指向仓库内的分区表，不再复制文件。

## 4. 词库装载

`CONFIG_LV_PINYIN_IME_DICT_SRC` 三选一：

| 取值 | 路径来源 | 说明 |
| --- | --- | --- |
| `PARTITION`（默认） | `esp_partition_mmap()` + 自写只读 VFS | 暴露为 `/ime/dict_pinyin.dat`；分区不存在时自动退回 EMBEDDED 副本 |
| `SDCARD` | `CONFIG_LV_PINYIN_IME_DICT_SD_PATH` | 默认 `/sdcard/dict_pinyin.dat` |
| `EMBEDDED` | 链接进 app 的二进制（`target_add_binary_data`） | 拷贝到 PSRAM 后经同一 VFS 暴露；8 MB 方案使用 |

词库不下载官方 `dict_pinyin.dat`：仓库内置上游原始词表，由仓库内的 dictbuilder
现场生成，结果可复现（1 073 858 B）：

```powershell
cd d:\Codes\lvgl_pinyin_ime
python tools\gen_dict.py            # 首次会自动编译 generated/dictbuilder
```

烧写词库与字体分区（分区表由 `idf.py flash` 写入，内容用 parttool）：

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM17 -Dict -Font
```

## 5. 字体

字体有两种来源，由 `CONFIG_LV_PINYIN_IME_FONT_SRC` 选择：

| 选项 | 行为 | 适用 |
|---|---|---|
| `EMBEDDED`（默认） | `generated/lv_font_ime_20.c` / `_16.c` 直接编进 app，直接引用 `lv_font_ime_20` / `lv_font_ime_16` | 开箱即用，**不用烧任何分区** |
| `PARTITION` | `font_partition.bin` 烧到 `font` 分区，运行时用 `lv_binfont_create_from_buffer()` 加载 | 想省 app 空间、或字体需要单独升级 |

### 5.1 EMBEDDED（默认）

`tools/gen_font.py --format c` 生成两个 C 文件，每个文件里是 `glyph_bitmap`
字节数组 + 一个以文件名命名的 `lv_font_t`。CMake 把它们加进源码列表，所以
**字体在固件里，键盘一上电就能显示汉字**，没有文件系统参与，也就没有"字体
加载失败 → 候选栏全是方框"这种静默故障。

实测体积（GB2312 + UI + ASCII，7 549 字，均 2 bpp）：

| 字体 | 规格 | 位图数据 | 最大 `bitmap_index` |
|---|---|---|---|
| `lv_font_ime_20` | 20 px / 2 bpp | 约 675 KB | 691 179 |
| `lv_font_ime_16` | 16 px / 2 bpp | 约 437 KB | 447 913 |

两者合计使 app 约 **2.9 MB**（8 MB app 分区）。

#### ⚠️ lv_font_conv 1.5.2 生成 LVGL 8 格式，`gen_font.py` 会补三处

上游 `lv_font_conv` 的最新版就是 1.5.2，它按 **LVGL 8** 的结构体写 C 文件，
直接喂给 LVGL 9 会**一个字都画不出来**（不是方框，是空白）。`gen_font.py`
在生成后修正下面三点，改动都有注释说明原因：

| 差异 | LVGL 8 / 生成器 | LVGL 9 | 不修的后果 |
|---|---|---|---|
| `lv_font_fmt_txt_dsc_t` 新字段 | 只有到 `bitmap_format` | 多了 `stride`、`are_glyphs_dynamic_loaded` | 后者非 0 时 LVGL 把 `glyph_bitmap` 当加载器对象 → 崩溃 |
| `lv_font_t` 新字段 | 无 | `static_bitmap` | `lv_font_get_glyph_static_bitmap()` 返回 NULL 并告警 |
| `bitmap_format` 取值 | 生成器对未压缩字体也写 `1` | `{PLAIN=0, COMPRESSED=1}` | 见下 |

关于 `bitmap_format` 有个反直觉的点：生成器**默认做 RLE 压缩**，此时写
`1`；LVGL 9 里 `COMPRESSED` 正好也是 `1`，所以**原样保留才是对的**。
`gen_font.py` 生成 C 时显式加 `--no-compress`，让位图变成裸数据、格式写 `0`，
只比压缩大约大 25%，但省掉了对压缩解码路径的依赖。

> 因此 **`CONFIG_LV_FONT_FMT_TXT_LARGE` 必须保持关闭**：`lv_font_conv` 只会写
> 紧凑的 8 字节 glyph 描述符（`bitmap_index:20`），打开大格式后 LVGL 按 12 字节
> 步长遍历 → 全部字形为空。它同时也是 1 MB 位图上限的来源，`gen_font.py` 会在
> 生成时校验并直接报错。`src/data/ime_font.c` 里有编译期 `#error` 拦住误开。

### 5.2 PARTITION（可选）

字体分区存 `font_partition.bin`，即 `lv_font_ime_20.bin`（候选/上屏正文）
+ `lv_font_ime_16.bin`（键面与拼音浮窗）的合并镜像，实测 2 413 KB。

运行时由 `src/data/ime_font.c` 加载：`esp_partition_mmap()` 拿到首字体长度后，
把字体镜像拷进 PSRAM，再交给 `lv_binfont_create_from_buffer()`。

> ⚠️ 选 PARTITION 时**必须打开 `CONFIG_LV_USE_FS_MEMFS`**（`sdkconfig.defaults`
> 里已设好 `CONFIG_LV_FS_MEMFS_LETTER=65`，即盘符 `A`）。这个 API 内部走 LVGL
> 文件系统，而 `lv_binfont_create(path)` 会把路径首字符当盘符解析——本组件给
> libgooglepinyin 装的只读 VFS 是 **ESP-IDF VFS**，LVGL 看不见，所以路径方式
> 永远打不开文件，只会让字体静默加载失败、候选栏显示方框。
> 忘了开这个选项时，`ime_font.c` 会在编译期用 `#error` 直接拦住。

### 5.3 生成流程

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1   # 只需一次，建 tools/venv
.\tools\venv\Scripts\python.exe tools\gen_charset.py           # 字表 + 覆盖率报告
.\tools\venv\Scripts\python.exe tools\gen_font.py              # 默认 --format c，写 C 数组
.\tools\venv\Scripts\python.exe tools\gen_font.py --format bin # 需要分区镜像时
.\tools\venv\Scripts\python.exe tools\check_charset.py         # 校验 UI 用到的字都在字表里
```

`check_charset.py` 会扫 `src/ui/*.c` 的字符串字面量和 `LV_SYMBOL_*`，**按下
LVGL 的规则遍历生成字体的 cmap**，报出任何"界面会画但字体里没有"的字符。
它同时检查两件事，因为只查一件会互相掩盖：

1. 字符在 `generated/charset.txt` 里吗；
2. 字符在**生成出来的字体**里吗——`lv_font_conv` 会**静默丢掉**源 TTF 里没有
   字形的符号。

> 实测：`DreamHanSansSC-W17.ttf` 里 **没有** U+232B `⌫` 和 U+2328 `⌨`，
> 所以退格键与键盘切换键的文案改成 `←` 与 `abc`。`tools/check_charset.py`
> 就是为发现这类问题写的：字表里有、字体里没有 = 屏上空白。

#### 空格必须单独用 `--range 32` 指定

`gen_charset.py` 是**一行一个字符**写 `charset.txt` 的，所以空格 U+0020 落下
来就是一个空行，读回时被 `split()` 丢掉。字表里没有空格 → 字体 cmap 从
U+0021 开始 → LVGL 在**每个空格处**找不到字形，于是画它的"缺字方框"（那个
细边框小方块），输入框占位文字和上屏文本里都会出现。

因此 `gen_font.py` 生成时固定加 `--range 32`（`--range` 不按空白切分，
`--symbols` 会）。修好后 cmap 与 LVGL 自带字体一致：`range_start = 32,
range_length = 95`。

字表默认 **GB2312 + UI 符号 + ASCII（7 549 字）**。引擎理论上还能输出 9 716 个
GB2312 之外的汉字，全量字表会让 20 px 字体溢出 1 MB 偏移上限，因此默认不含
（这些字会显示为缺字方框）。需要时：

```powershell
.\tools\venv\Scripts\python.exe tools\gen_charset.py --with-extra-hanzi
.\tools\venv\Scripts\python.exe tools\measure_fonts.py          # 先看体积
```

`tools/inspect_font.py` 可校验生成物是否为合法的 LVGL 二进制字体容器：

```powershell
.\tools\venv\Scripts\python.exe tools\inspect_font.py generated\lv_font_ime_20.bin
```

### 5.4 键盘外观

按键是**方角小圆角**，不是胶囊形。`src/ui/lv_pinyin_ime_style.c` 里
`IME_KEY_RADIUS` 为 4 px：LVGL 会把圆角钳到短边的一半，26 px 宽的键一旦超过
13 px 就会被画成椭圆。键高由 `IME_UI_ROW_HEIGHT`（34 px）决定。

按键样式：白底 + `IME_KEY_BORDER`（1 px）`#B6BDC8` 细边框，浮在 `#D3D8E0` 的
深一档底板上，所以每个键读起来是独立的按钮而不是一整片。

#### 键宽由 flex 权重决定，不是像素

每个键用 `lv_obj_set_flex_grow(btn, width_u)` 而不是像素宽度，行内按键按权重
瓜分整行宽度。这样做的原因有两个，都是踩过的坑：

1. **两端的留白**。按像素给宽度时，12 个键加起来只有 279 px，而行框有 316 px，
   剩下的 37 px 就变成左右空白。权重分配让每行**恰好填满**。
2. **间隙不均匀**。逐个算像素宽度再指望它们加起来等于行宽，结果是每个键后面的
   间隙都不一样（实测一个是 8 px、其余 3 px）。权重分配没有这个问题。

26 键的字母键是 0.9u、外侧功能键（符 / ⇧ / 分词 / ⌫ / ⏎）是 1.5u。把外侧键
做得比字母宽，既符合参考图，也正好把这 12 个单位填满。

> ⚠️ `ime_style_apply_key()` 的第一句是 `lv_obj_remove_style_all()`，LVGL 这个
> 调用**连对象上的本地属性一起清掉**。所以必须先套样式、再设尺寸/权重，反过来
> 宽度会被丢掉、按键退化成按内容撑开（每个键宽窄不一）。候选键同理。

#### 特殊键用 LVGL 自带符号字体

退格、回车、确认、键盘切换、Shift 都用 LVGL 的符号字形
（`LV_SYMBOL_BACKSPACE` / `_NEW_LINE` / `_OK` / `_KEYBOARD` / `_UP`），与
LVGL 自带 keyboard 的做法一致。这些字形位于 FontAwesome 私用区，由内置
Montserrat 字体提供，**子集化的中文字体里没有**，所以
`src/ui/lv_pinyin_ime_kb.c` 的 `key_caption_font()` 给这些键的标签用
`LV_FONT_DEFAULT`，其余标签用 IME 字体。

> 不要把符号加进生成的 CJK 字体：字表来自源 TTF，里面根本没有这些字形
> （`tools/check_charset.py` 会跳过 `LV_SYMBOL_*`）。

#### 九键（T9）的拼音候选必须是真的拼音

九键把数字串切分成音节，候选拼音行的每一项都应该是**真实存在的拼音**。这里踩过
两个坑，都会在屏幕上显示不可拼读的候选（例如按下 `32` 出现 `e'a`）：

1. **按前缀匹配音节**。`seg()` 原本拿缓冲的一个**前缀**去和音节表比，于是
   `"32"` 会匹配到某个"数字串以 32 开头"的更长音节，凭空造出 `ea`。
   音节表里只有完整音节，所以**按音节自身的完整长度比较**才是对的。
2. **把单字母音节串起来**。词表里确实有 `锕 a`、`唔 n`、`呣 m` 这类单字母音节，
   单用是合法的，但 `32` 可以被读成 `e`+`a`（`e` 在 3 键、`a` 在 2 键）——
   键位读法没错，可没人会这么打字。所以**单字母音节只在它独占整个缓冲时才允许**。

`ime_t9_is_valid()` 按音节表校验一串拼音；`host_core_test` 用它做两件事：逐条检查
多组数字串产生的每个候选，以及校验器自身（`da`/`xi'an` 通过，`ea`/`da'ea` 拒绝）。
这组断言有 60 项，就是防这两个坑回归的。

音节表本身（`src/generated/ime_t9_syllables.h`，413 条）由 `tools/gen_t9_table.py`
从引擎自己的词表生成，所以"内置拼音表"和词典永远一致，不会出现引擎不认识的拼写。

#### 空格键

有拼音缓冲或候选列表时，空格**上屏当前高亮的候选词**；半途未成词则把字母原样
送出；完全没有组合时插入一个真正的空格。见 `src/ui/lv_pinyin_ime.c` 的
`IME_KEY_SPACE`。

26 键第 4 行顺序为 `?123 | ⌨ | 空格(4u) | 中 | ✓`：空格居中在拇指位置并与上方
字母列对齐，两个模式键留在两角。

#### 中英切换键

只显示**当前生效**的一侧，单字：中文 `中`、英文 `英`。它是 `key_label()` 里的
动态文案而不是键表条目，切换语言会重建键盘。

## 5.5 主机端渲染检查（不用烧板看布局）

`test_apps/host_ui_render` 用真实 LVGL 把 IME 画出来并导出 PPM，用来在没有硬件时
确认键位布局与可见性：

```powershell
cmake -S test_apps/host_ui_render -B build/host_ui_render -G Ninja
cmake --build build/host_ui_render
.\build\host_ui_render\host_ui_render.exe build\ime_k26.ppm k26   # 也可 k9 / p1 / en
```

输出包含根对象坐标、各键行的位置与按钮数，以及 `visible children` 计数。

## 6. 扩展词库（SD 卡，可选）

- 开关：`CONFIG_LV_PINYIN_IME_EXT_DICT_ENABLE`
- 路径：`CONFIG_LV_PINYIN_IME_EXT_DICT_PATH`，默认 `/sdcard/ime_ext.bin`
- 生成：`tools/gen_extdict.py`，格式见 [dict_format.md](./dict_format.md)

拔出 SD 卡不影响基本功能（装载失败时只打印警告并继续）。

## 7. 应用侧最小用法

```c
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

lv_obj_t *ime = lv_pinyin_ime_create(lv_screen_active());
lv_obj_t *ta  = lv_textarea_create(lv_screen_active());

lv_pinyin_ime_attach(ime, ta);                           /* 上屏目标 */
lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K26);     /* 26 键 / 九键 */
lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_CN);

/* 点 ✓ 且拼音缓冲为空时抛出的事件，应用侧据此收起键盘 */
lv_obj_add_event_cb(ime, on_ime_ready, lv_pinyin_ime_event_ready(), NULL);
```

### 7.1 示例工程的屏幕（ST7789）

`examples/k26_k9_demo` 已接好 ST7789（SPI 接口，320 x 240 横向），使用
ESP-IDF 自带的 `esp_lcd` 驱动，未引入额外部件。引脚与参考工程
`ESP32_MusicPlayer_V4` 一致：

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| SCLK | 12 | SPI2 时钟 |
| MOSI | 11 | SPI2 数据 |
| MISO | 13 | 复用给后面的 XPT2046 触摸 |
| CS | 10 | 屏幕片选 |
| DC | 9 | 数据/命令 |
| RST | 14 | 复位 |
| BL | 4 | 背光，低电平点亮 |

其他参数：SPI 时钟 27 MHz、像素格式 RGB565、元素顺序 BGR、
`esp_lcd_panel_invert_color(false)`（对应参考工程的 `TFT_INVERSION_OFF`）。
面板原生 240 x 320，通过 `esp_lcd_panel_swap_xy(true)` 转成 320 x 240 横向；
若实际成像镜像或上下颠倒，调整 `main.c` 中 `esp_lcd_panel_mirror()` 的两个参数。

LVGL 与屏幕之间直接用 `lv_display_set_flush_cb()` + `esp_lcd_panel_draw_bitmap()` 对接，
传输完成回调里调用 `lv_display_flush_ready()`，渲染缓冲固定放在内部 RAM
（SPI DMA 不能从 PSRAM 取数）。

显示缓冲用 `lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED)`：
ST7789 是大端器件，LVGL 默认按小端渲染，不切这一下颜色就是错乱的（DMA 送出去
的字节顺序反了），文字抗锯齿边缘也会串色。

XPT2046 触摸（CS = 15，IRQ = 40 不接）共用这条 SPI 总线，由
`main/touch_xpt2046.c` 轮询读取，注册为 LVGL 的指针输入设备（2 MHz、SPI mode 0）。
读出的 19 个字节按「命令字节 + 哑字节」成组，每组拼成 16 位后右移 3 位取 12 位结果；
**拼装顺序必须与 Arduino 的 `SPI.transfer16()` 一致**（哑字节在高位），
否则会把上一次转换的尾部位混进来，读出不可能是 12 位的值（>4095）。
判据：正确的拼装里，高字节的最高位（转换的 busy 位）恒为 0。
`touch_xpt2046_init()` 里带一次自检：若 19 个字节全相同（0x00/0xFF），说明控制器没有
驱动 MISO，直接给出接线提示；否则打印前 4 个字节，说明总线是通的。

`main/touch_cal.c` 是校准向导，串口敲 `c` 启动、`x` 取消，四点采样后线性外推到屏幕
四边，结果写入 NVS 的 `touch` 命名空间（`xl`/`xr`/`yt`/`yb` 加 `ver` 版本号），开机自动读回。
**标定与读取路径绑定**：`ver` 不匹配时直接忽略存档并回退到出厂默认值，避免旧标定
把每次点击都映射到错误位置（表现成“触摸完全没反应”）。
校准入口放串口而不是屏幕按钮，是为了避免“标定不准 → 点不中校准按钮”的死锁。
完整命令表见 [usage.md](./usage.md) 第 5 节。

## 8. 构建验证

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd d:\Codes\lvgl_pinyin_ime\examples\k26_k9_demo
idf.py set-target esp32s3       ; idf.py build
idf.py --preview set-target esp32s31 ; idf.py --preview build
idf.py set-target esp32p4       ; idf.py build
```

标准：三个 target 均输出 `Project build complete`，且无本组件产生的警告。

说明：ESP-IDF 6.1 中 `esp32s31` 仍标记为 preview，缺少 `--preview` 会被拒绝。

首次构建前若尚未创建 IDF 6.1 的 Python 环境，需先执行一次
`& "D:\Espressif\EIM\v6.1\esp-idf\install.ps1"`（安装器末尾会提示
`Invalid feature specifier: -IdfTarget`，该报错不影响 Python 环境创建）。