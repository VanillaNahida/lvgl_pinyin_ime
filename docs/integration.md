# 组件接入说明

把 `lvgl_pinyin_ime` 作为组件接进你自己的 ESP-IDF 工程：声明依赖 → 配分区表 →
选词库来源 → 选字体来源 → 写少量应用代码。

只想先跑通示例，看 [usage.md](./usage.md)；键位与 API 细节见
[keymap.md](./keymap.md) 与 [include/lvgl_pinyin_ime/](../include/lvgl_pinyin_ime/)。

---

## 1. 环境

| 项 | 值 |
| --- | --- |
| ESP-IDF | 6.1（路径换成你自己的，下同） |
| LVGL | 9.5（组件仓库 `lvgl/lvgl`，首次构建联网拉取到 `managed_components/`） |
| 目标芯片 | ESP32-S3（主）、ESP32-P4；ESP32-S31 为 preview |
| 主机端 Python | 生成词库/字体用，一律放在 `tools/venv`（`tools/setup_env.ps1` 创建） |
| Node.js | LTS，`lv_font_conv` 依赖 |

---

## 2. 作为组件引入

### 2.1 本仓库内的示例工程

`examples/k26_k9_demo/CMakeLists.txt` 通过 `EXTRA_COMPONENT_DIRS` 把仓库根目录注册为
一个 IDF 组件，同时也注册了 vendored 的引擎组件：

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../src/third_party/libgooglepinyin"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(k26_k9_demo)
```

注意 `src/third_party/libgooglepinyin` 必须一起注册，否则 `REQUIRES libgooglepinyin`
会解析失败。

### 2.2 独立工程引入

在你的工程里让 `main` 依赖它。`main/idf_component.yml`：

```yaml
dependencies:
  idf: ">=5.3"
  lvgl/lvgl: "^9.5"
  lvgl_pinyin_ime:
    version: "*"          # 已发布到组件仓库时
    # 开发期用本地路径覆盖，二选一：
    # override_path: "../../lvgl_pinyin_ime"
```

`main/CMakeLists.txt`：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl lvgl_pinyin_ime
)
```

然后 `idf.py reconfigure` 会拉取/链接组件。

> 用 `override_path` 指向本仓库时，`libgooglepinyin` 是仓库内的子组件，组件管理器会
> 一并解析。若直接 copy 本仓库到工程里，记得连同 `src/third_party/` 一起带。

---

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

合计约 13.6 MB < 16 MB。`partitions/ime_8mb.csv`：4 MB app、无独立 font 分区、
字体内嵌为 C 数组。

工程侧配置（示例工程的 `sdkconfig.defaults` 中该值为 `"../../partitions/ime_16mb.csv"`，
路径**相对于工程目录**，所以直接指向仓库内的分区表，不必复制文件）：

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/ime_16mb.csv"
```

> 分区名 `dict` / `font` / `usr` 目前取自代码里的常量（`src/data/ime_dict.c`、
> `src/data/ime_font.c`），`Kconfig` 里的 `LV_PINYIN_IME_DICT_PARTITION` 等同名选项
> 尚未接线。分区表里的名字要与代码一致。

---

## 4. 词库装载

`CONFIG_LV_PINYIN_IME_DICT_SRC` 三选一：

| 取值 | 路径来源 | 说明 |
| --- | --- | --- |
| `PARTITION`（默认） | `esp_partition_mmap()` + 自写只读 VFS | 暴露为 `/dict/dict_pinyin.dat`；分区不存在时自动退回内嵌副本 |
| `SDCARD` | `CONFIG_LV_PINYIN_IME_DICT_SD_PATH` | 默认 `/sdcard/dict_pinyin.dat` |
| `EMBEDDED` | 链接进 app 的二进制（`target_add_binary_data`） | 拷进 PSRAM 后经同一 VFS 暴露；8 MB 方案使用 |

词库不下载官方 `dict_pinyin.dat`：仓库内置上游原始词表，由仓库内的 dictbuilder 现场
生成，结果可复现（1 073 858 B）：

```powershell
cd <仓库根>
python tools\gen_dict.py            # 首次会自动编译 generated/dictbuilder
```

烧写词库与字体分区（分区表由 `idf.py flash` 写入，内容用 parttool）：

```powershell
& "<你的 IDF 路径>\export.ps1"
powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM5 -Dict -Font
```

> SD 卡**扩展词库**（在大词库之外再挂一份自定义词表）尚未落地：`Kconfig` 里的
> `LV_PINYIN_IME_EXT_DICT_ENABLE` / `LV_PINYIN_IME_EXT_DICT_PATH` 目前没有对应代码，
> 格式约定见 [dict_format.md](./dict_format.md) 第 1 节（属规划中的格式）。

---

## 5. 字体

字体有两种来源，由 `CONFIG_LV_PINYIN_IME_FONT_SRC` 选择：

| 选项 | 行为 | 适用 |
| --- | --- | --- |
| `EMBEDDED`（默认） | `generated/lv_font_ime_20.c` / `_16.c` 直接编进 app，引用 `lv_font_ime_20` / `lv_font_ime_16` | 开箱即用，**不用烧任何分区** |
| `PARTITION` | `font_partition.bin` 烧到 `font` 分区，运行时用 `lv_binfont_create_from_buffer()` 加载 | 想省 app 空间、或字体需要单独升级 |

### 5.1 EMBEDDED（默认）

`tools/gen_font.py --format c` 生成两个 C 文件，每个文件里是 `glyph_bitmap` 字节数组
加一个以文件名命名的 `lv_font_t`。CMake 把它们加进源码列表，所以**字体在固件里，
键盘一上电就能显示汉字**，没有文件系统参与，也就没有"字体加载失败 → 候选栏全是方框"
这种静默故障。

实测体积（GB2312 + UI + ASCII，7 549 字，均 2 bpp）：

| 字体 | 规格 | 位图数据 |
| --- | --- | --- |
| `lv_font_ime_20` | 20 px / 2 bpp | 约 675 KB |
| `lv_font_ime_16` | 16 px / 2 bpp | 约 437 KB |

### 5.2 PARTITION（可选）

字体分区存 `font_partition.bin`，即 `lv_font_ime_20.bin` + `lv_font_ime_16.bin` 的合并
镜像。运行时由 `src/data/ime_font.c` 加载：`esp_partition_mmap()` 拿到首字体长度后，
把镜像拷进 PSRAM，再交给 `lv_binfont_create_from_buffer()`。

必须同时打开 `CONFIG_LV_USE_FS_MEMFS`（示例 `sdkconfig.defaults` 里已设
`CONFIG_LV_FS_MEMFS_LETTER=65`，即盘符 `A`）。这个 API 内部走 LVGL 文件系统，
而 `lv_binfont_create(path)` 会把路径首字符当盘符解析 —— 本组件给 libgooglepinyin
装的只读 VFS 是 ESP-IDF VFS，LVGL 看不见，所以路径方式永远打不开文件，只会让字体
静默加载失败、候选栏显示方框。忘了开这个选项时，`ime_font.c` 会在编译期 `#error` 拦住。

### 5.3 生成流程

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1   # 只需一次，建 tools/venv
.\tools\venv\Scripts\python.exe tools\gen_charset.py           # 字表 + 覆盖率报告
.\tools\venv\Scripts\python.exe tools\gen_font.py              # 默认 --format c，写 C 数组
.\tools\venv\Scripts\python.exe tools\gen_font.py --format bin # 需要分区镜像时
.\tools\venv\Scripts\python.exe tools\check_charset.py         # 校验 UI 用到的字都在字表里
```

`check_charset.py` 会扫 `src/ui/*.c` 的字符串字面量和 `LV_SYMBOL_*`，按下 LVGL 的
规则遍历生成字体的 cmap，报出任何"界面会画但字体里没有"的字符。它同时检查两件事，
因为只查一件会互相掩盖：字符在 `generated/charset.txt` 里吗；以及字符在**生成出来的
字体**里吗（`lv_font_conv` 会静默丢掉源 TTF 里没有字形的符号）。

字表默认 **GB2312 + UI 符号 + ASCII（7 549 字）**。字表里没有的字会显示为缺字方框；
需要更全的字表时：

```powershell
.\tools\venv\Scripts\python.exe tools\gen_charset.py --with-extra-hanzi
.\tools\venv\Scripts\python.exe tools\measure_fonts.py          # 先看体积
```

`tools/inspect_font.py` 可校验生成物是否为合法的 LVGL 二进制字体容器：

```powershell
.\tools\venv\Scripts\python.exe tools\inspect_font.py generated\lv_font_ime_20.bin
```

### 5.4 三个必须记住的约束

**`CONFIG_LV_FONT_FMT_TXT_LARGE` 必须保持关闭。** `lv_font_conv` 只会写紧凑的 8 字节
glyph 描述符（`bitmap_index:20`）；打开大格式后 LVGL 按 12 字节步长遍历，结果**一个字
都画不出来**（不是方框，是空白）。它同时也是 1 MB 位图上限的来源，`gen_font.py` 会在
生成时校验并直接报错，`src/data/ime_font.c` 里有编译期 `#error` 拦住误开。

**空格必须单独用 `--range 32` 指定。** `gen_charset.py` 是一行一个字符写 `charset.txt`
的，空格落下就是一个空行，读回时被丢掉。字表里没有空格 → cmap 从 U+0021 开始 →
LVGL 在每个空格处画缺字方框。因此 `gen_font.py` 生成时固定加 `--range 32`。

**上面两点 `gen_font.py` 已经处理好了**，列在这里是为了在你想改工具链时知道它们存在。

---

## 6. 应用侧最小用法

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

以下三者在建控件**之前**调用（都是建控件时读取的全局状态）：

| 函数 | 作用 |
| --- | --- |
| `lv_pinyin_ime_set_fit_height(px)` | 按目标高度自适应行高，键盘占屏幕一半时最省心 |
| `lv_pinyin_ime_set_row_height(px)` | 需要固定行高时使用 |
| `lv_pinyin_ime_set_fonts(big, small)` | 用应用自己的中文字体，字更全；`NULL` 表示该尺寸沿用内置 |

用自带字体时**不要**把 LVGL 的符号字形（`LV_SYMBOL_BACKSPACE` / `_NEW_LINE` / `_OK` /
`_KEYBOARD` / `_UP`，位于 FontAwesome 私用区）加进生成的中文字体：字表来自源 TTF，
里面没有这些字形。退格、回车等特殊键的标签走 LVGL 自带符号字体，其余标签走 IME 字体，
组件已按此处理。

其余公开 API（`lv_pinyin_ime_reset` / `_show` / `_hide` / `_set_panel` / `_dump`、
事件码 `lv_pinyin_ime_event_cand_changed()` 等）见
[include/lvgl_pinyin_ime/lv_pinyin_ime.h](../include/lvgl_pinyin_ime/lv_pinyin_ime.h)。
语音输入见 [voice.md](./voice.md)。

---

## 7. 构建验证

```powershell
& "<你的 IDF 路径>\export.ps1"
cd <仓库根>\examples\k26_k9_demo
idf.py set-target esp32s3            ; idf.py build
idf.py --preview set-target esp32s31 ; idf.py --preview build
idf.py set-target esp32p4            ; idf.py build
```

标准：三个 target 均输出 `Project build complete`，且无本组件产生的警告。
ESP-IDF 6.1 中 `esp32s31` 仍标记为 preview，缺少 `--preview` 会被拒绝。

不需要硬件的编译检查（针对某目标，不烧板）见
[README 的验证一节](../README_zh-cn.md#验证不需要硬件)。

### 7.1 `idf.py fullclean` 不会清掉 `build/config/sdkconfig.h`

症状很迷惑：把 `sdkconfig` 里的某个 `CONFIG_` 改回关闭、跑 `idf.py build`，结果对应
文件仍被编译 —— 配置里明明写着"没开"。

原因：`fullclean` 清的是目标文件与生成物，`build/config/` 下那份生成出来的
`sdkconfig.h` 会留下；它比 `sdkconfig` 新，CMake 的"输入比输出新"判断认为不需要重新
生成，于是编译器继续看到旧的宏。处理：改完 `sdkconfig` 若行为不符合预期，直接删掉整个
`build/` 再构建。判断当前真实配置最快的办法：

```powershell
Select-String -Path build\config\sdkconfig.h -Pattern 'LV_PINYIN_IME'
```
