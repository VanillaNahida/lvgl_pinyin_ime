# 使用指南

从零把这套东西跑起来：环境准备 → 接线 → 编译烧录 → 触摸校准 → 在自己的工程里引用。

---

## 1. 这个项目是什么

一个面向 **ESP-IDF 6.1 + LVGL 9.5** 的中文拼音输入法组件 `lvgl_pinyin_ime`，
可以作为组件被别的工程引用，也可以直接烧 `examples/k26_k9_demo` 看效果。

- 目标芯片：ESP32-S3-N16R8（主）、ESP32-S31、ESP32-P4
- 规划能力：26 键 / 九键、全拼 / 双拼、词组、分词、联想、用户词频学习、模糊音
- 引擎：移植 libgooglepinyin

### 当前进度（重要）

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| S-1 | 设计文档 | 已完成 |
| S0 | 工程骨架 + 示例工程 + 三目标编译 | 已完成 |
| S1 | libgooglepinyin 移植、引擎门面、词库装载 | 已完成（主机端单测通过） |
| S2 | Python 工具链（词库 / 字表 / 字体） | 已完成（`gen_dict` / `gen_charset` / `gen_font` / `measure_fonts` / `inspect_font`） |
| S3 | 26 键 UI 主体 | 已完成（待上板目视确认） |
| S4 | 九键 | 已完成（待上板目视确认） |
| S5 | 双拼 / 模糊音 / 联想 / 用户词库 | 未开始 |
| S6 | 主机端单测、文档收尾 | 进行中（`test_apps/host_core_test` 已可用） |

已可运行的两条验证路径：

```powershell
# 1) 主机端引擎 + T9 + 哈希/前缀索引单测（不需要硬件）
cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
cmake --build build/host_core_test
.\build\host_core_test\host_core_test.exe data/dict/dict_pinyin.dat

# 2) 组件编译检查（不需要硬件）
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd test_apps\idf_component_build
idf.py set-target esp32s3 ; idf.py build
```

词库不再需要联网下载官方 `dict_pinyin.dat`：仓库内置了上游原始词表，
`python tools/gen_dict.py` 会用仓库内的 dictbuilder 现场生成（见第 10 节）。

三目标编译检查的实测结果（2026-10，ESP-IDF v6.1）：

| target | 结果 |
| --- | --- |
| esp32s3 | `Project build complete`（镜像约 1.5 MB，含 1 MB 内嵌词库回退） |
| esp32s31 | `Project build complete` |
| esp32p4 | `Project build complete` |

---

## 2. 环境准备

| 需要什么 | 说明 |
| --- | --- |
| ESP-IDF 6.1 | 本机路径 `D:\Espressif\EIM\v6.1\esp-idf` |
| LVGL 9.5 | 不用手动装，首次构建时组件管理器会自动拉到 `managed_components/`（需联网） |
| Node.js LTS | 只有 S2 阶段生成字体时才用得到 |
| Python 依赖 | 一律用 `tools/venv`，**不要装到全局**（见第 10 节） |

### 2.1 首次使用前先初始化 IDF 的 Python 环境

本机只有 5.4 / 5.5 的 IDF Python 环境，6.1 的需要先建一次：

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\install.ps1"
```

脚本最后会报一句 `ERROR: Invalid feature specifier: -IdfTarget`，**这是无害的**，
Python 环境已经建好了（`D:\Espressif\python_env\idf6.1_py3.12_env`）。
看到 `All done! You can now run: export.ps1` 就说明成功。

### 2.2 每个新开的终端都要先 export

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
```

`idf.py`、`esptool` 这些命令都由它注入到当前会话，不 export 就会提示找不到命令。

---

## 3. 硬件接线

参考工程 `ESP32_MusicPlayer_V4` 的板子，屏幕与触摸共用一条 SPI2 总线，各自用不同片选。

### 3.1 ST7789 屏幕（320 x 240，横向）

| 屏幕信号 | ESP32 引脚 | 备注 |
| --- | --- | --- |
| SCLK | GPIO12 | SPI2 时钟 |
| MOSI / SDA | GPIO11 | SPI2 数据 |
| MISO | GPIO13 | 屏幕不用，给触摸复用 |
| CS | GPIO10 | 屏幕片选 |
| DC / RS | GPIO9 | 数据/命令 |
| RST | GPIO14 | 复位 |
| BL | GPIO4 | 背光，**低电平点亮** |
| VCC / GND | 3.3V / GND | |

### 3.2 XPT2046 触摸

| 触摸信号 | ESP32 引脚 | 备注 |
| --- | --- | --- |
| CS | GPIO15 | 触摸片选 |
| IRQ | GPIO40 | **本项目不接**，走轮询 |
| 其余信号 | 与屏幕共用 SCLK / MOSI / MISO | |

屏幕侧的参数已经按参考工程配好：SPI 27 MHz、RGB565、BGR、不反色。
触摸侧 2 MHz、轮询。

---

## 4. 编译、烧录、看日志

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd d:\Codes\lvgl_pinyin_ime\examples\k26_k9_demo

idf.py set-target esp32s3        # 只需做一次；换芯片时再改
idf.py build
idf.py -p COM5 flash monitor     # COM5 换成你的实际串口
```

- 第一次 `set-target` / `build` 需要联网（拉 LVGL 9.5），会慢一些
- `Ctrl + ]` 退出 monitor
- 想换芯片：`idf.py set-target esp32p4`；**S31 属于 preview**，
  必须写 `idf.py --preview set-target esp32s31`，否则会被直接拒绝
- 编译不过又怀疑缓存时：`idf.py fullclean` 后重新 `idf.py build`

正常启动后串口会打印类似：

```
I (xxx) xpt2046: XPT2046 attached to SPI2_HOST (id 1), CS=15, 2000000 Hz
I (xxx) xpt2046: controller answers, first bytes: C1 18 91 00      <- 自检
I (xxx) xpt2046: calibration loaded from NVS: x=... y=...          <- 有存档时才打印
I (xxx) k26_k9_demo: lvgl_pinyin_ime demo started, 320x240 ST7789 + XPT2046, mode=0 lang=0
I (xxx) k26_k9_demo: commands: c = calibrate, x = abort, t = show calibration,
```

屏幕上方还有一行 `tap N at (x, y)`，每点一次屏幕就会变一次——
**这是判断触摸通不通最直接的办法**：

| 现象 | 结论 |
| --- | --- |
| 点屏幕数字不涨 | 触摸链路有问题（看启动自检和下面的 5.4 节） |
| 数字涨但坐标很离谱 | 触摸是通的，只是标定不对，敲 `c` 校准 |
| 点上半屏涨、下半屏不涨 | 有控件把点击吃掉了 |

如果自检打印的是 `all 19 bytes are 0x00/0xFF: the controller is not driving MISO`，
说明控制器没在上拉/驱动 MISO，属接线或供电问题（CS=GPIO15、MISO=GPIO13、3.3V、GND）。

---

## 5. 触摸校准（第一次上板基本都要做一次）

触摸走的是**原始值线性映射**，出厂默认值是从参考工程那套实测标定换算过来的，
换一块屏或换一个安装方向就不可能准，所以上板第一件事是校准。

校准入口刻意放在**串口**而不是屏幕按钮上：标定离谱的时候，屏幕上的按钮恰恰点不中，
“用错的标定去校准标定”会变成死锁。校准采样读的是**原始值**，不经过当前标定，
所以无论旧标定多离谱都能校。

### 5.1 串口命令

用 `idf.py -p COM5 monitor` 打开串口后，直接敲单个字符即可（不用回车）：

| 命令 | 作用 |
| --- | --- |
| `c` | 启动校准，屏幕上依次出现 4 个十字标记 |
| `x` | 取消正在进行的校准 |
| `t` | 打印当前标定，以及一次实测的原始值（按住屏幕再敲） |
| `r` | 开关原始值连续输出，用来诊断“触摸到底有没有响应” |
| `d` | 转储一次 SPI 事务的 19 个原始字节，用于排查字节序/对齐 |
| `n` | 删除 NVS 里存的标定，回到出厂默认值 |
| `h` / `?` | 帮助 |

### 5.2 校准流程

1. 敲 `c`。屏幕出现半透明遮罩 + 中间提示框，左上角出现红色十字
2. 手指按住十字中心约 0.2 秒再松开，串口打印 `point 1: raw=(...) from N samples`
3. 依次完成左上 → 右上 → 左下 → 右下四点
4. 四点采完自动换算、立即生效，并**写入 NVS**，屏幕显示 `Calibration stored`
5. 松开后就能正常操作了；重启自动从 NVS 读回，不用重复校准

校准数据存在 NVS 的 `touch` 命名空间（键 `xl`/`xr`/`yt`/`yb`，外加一个 `ver` 版本号），
用的是分区表里的 `nvs` 分区，和参考工程一致。

**版本号的作用**：标定值和读取路径是绑定的。一旦读取路径变了（比如修了 SPI 字节序），
旧标定就是错的——它会让每一次点击都落到错误的位置，看起来就像“触摸完全没反应”。
所以 `ver` 对不上时驱动会直接忽略存档、回退到出厂默认值，并打印：

```
W (xxx) xpt2046: stored calibration is from an older build (version 0, want 2), ignoring it
```

看到这行就是自动降级成功了，重新敲 `c` 校准一次即可覆盖旧数据。
想手动清掉存档就敲 `n`。

### 5.3 校准失败 / 触摸不响应怎么办

| 串口现象 | 含义 | 处理 |
| --- | --- | --- |
| `the two axes look exchanged` | 横竖两轴对调了，采样跨度异常 | 把 [touch_xpt2046.c](../examples/k26_k9_demo/main/touch_xpt2046.c) 顶部 `TOUCH_SWAP_AXES` 改成 `1`，重新编译再校准 |
| `calibration failed` | 采样值跨度太小 / 不合理 | 重新敲 `c`，手指按准十字中心、按住久一点 |
| `stored calibration is from an older build ... ignoring it` | NVS 里的标定是旧版本代码写的，已自动忽略 | 正常现象，重新敲 `c` 校准覆盖即可 |
| 启动时 `all 19 bytes are 0x00/0xFF` | 控制器没有驱动 MISO | 接线/供电问题：CS=GPIO15、MISO=GPIO13、3.3V、GND |
| 敲 `r` 后每 250 ms 一行 `raw=(...) z=...` | 控制器在响应 | 问题只是标定，按 5.2 校准即可 |
| 敲 `r` 后按住屏幕 `z` 恒为 0 | 读到的是无效数据 | 查接线：CS=GPIO15、MISO=GPIO13、3.3V/GND；确认启动自检那两行 |
| 敲 `r` 后一行都不打印 | 驱动没起来 | 看启动日志里有没有 `XPT2046 attached to SPI2_HOST` |
| 屏幕上 `tap N at (x, y)` 点一次涨一次 | 触摸链路正常 | 只是标定问题，敲 `c` |

### 5.4 关于 `r` 打出来的“随机数字”

**没摸屏幕时 `raw` 是噪声，随机跳是正常的**——电阻屏没被按下时 ADC 读数本来就没有意义，
判断“有没有触摸”要看 `z`：

- `z > 0` 且数值稳定 → 控制器正常工作
- `z` 恒为 0 → 这一帧没有触摸（不是故障）
- 按住屏幕后 `z` 仍然恒为 0 → 才是真的有问题

按住屏幕时，`raw` 应该在 200~3900 之间随手指位置单调变化。
**任何 `raw` 超过 4095 都是不可能的**（XPT2046 是 12 位），看到超过就说明
SPI 读出的字节拼装对齐错了——用 `d` 命令把 19 个字节打出来定位，
判据是「哪个拼装方式的高字节最高位恒为 0」，因为那个位置是转换的 busy 位。

---

## 6. 显示不对怎么办

| 现象 | 原因 | 怎么改 |
| --- | --- | --- |
| 全黑 | 背光没亮 / 没复位 | 检查 GPIO4 电平（低电平点亮）、RST 接线 |
| 颜色发灰、发虚、像负片 | ST7789 模块的反色要求不同 | [main.c](../examples/k26_k9_demo/main/main.c) 里把 `esp_lcd_panel_invert_color(panel, false)` 改成 `true` |
| 红蓝互换（绿变橙、红变蓝） | 面板色彩顺序是 RGB 而不是 BGR | 把 `panel_cfg.rgb_ele_order` 从 `LCD_RGB_ELEMENT_ORDER_BGR` 改成 `LCD_RGB_ELEMENT_ORDER_RGB` |
| 画面镜像 / 上下颠倒 | 面板安装方向不同 | 改 `esp_lcd_panel_mirror(panel, true, false)` 的两个参数 |
| 画面是竖的（240x320） | 没交换轴 | 确认 `esp_lcd_panel_swap_xy(panel, true)` 在调用 |
| 边缘有偏移/彩线 | 面板有 gap | `esp_lcd_panel_set_gap(panel, x, y)` |

### 6.1 颜色字节序（已修）

ST7789 是**大端**器件，要求每个 RGB565 像素先发高字节；LVGL 内部是**小端**存储。
两边直接对接会把字节顺序搞反，表现就是颜色错乱（绿色变橙、红色变蓝、
抗锯齿的文字边缘出现彩色毛刺，整片字看着发虚）。

本项目在 [main.c](../examples/k26_k9_demo/main/main.c) 里用

```c
lv_display_set_color_format(s_lv_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
```

让 LVGL 直接按大端渲染，颜色与文字锐度都恢复正常，且不额外消耗 CPU
（不是发送前再遍历一遍交换字节）。

如果你自己接屏幕时忘了这一句，就会看到上面那些症状；另一条等效路径是在
flush 回调里把缓冲逐像素交换，但那样每帧都要多跑一遍内存。

---

## 7. 在自己的工程里引用这个组件

最简单的方式是在自己工程的 `CMakeLists.txt` 里把它加进搜索路径：

```cmake
set(EXTRA_COMPONENT_DIRS "D:/Codes/lvgl_pinyin_ime")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

然后在 `main/idf_component.yml` 里声明 LVGL：

```yaml
dependencies:
  idf: ">=5.3"
  lvgl/lvgl: "^9.5"
```

`main/CMakeLists.txt` 里加上依赖：

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES lvgl lvgl_pinyin_ime)
```

代码里：

```c
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

lv_obj_t *ta  = lv_textarea_create(lv_screen_active());
lv_obj_t *ime = lv_pinyin_ime_create(lv_screen_active());

lv_pinyin_ime_attach(ime, ta);                                  /* 上屏目标 */
lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K26);            /* 26 键 / 九键 */
lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_CN);             /* 中文 / 英文 */
lv_obj_add_event_cb(ime, on_ready, lv_pinyin_ime_event_ready(), NULL);
```

### 7.1 用应用自己的字库（推荐，字更全）

组件自带的两套字体只覆盖 `tools/gen_charset.py` 选出的字符集（GB2312 + 十来个
UI 符号），键盘上的说明文字一旦超出这个集合就是**缺字方框** —— 而同一个字在别的
页面（用应用自己的字库）显示得好好的，看着就像"输入法的字库坏了"。

应用如果已经有 CJK 字库（整机 UI 位图字体、FreeType 字体……），直接让输入法用它：

```c
/* 必须在任何 lv_pinyin_ime_create() 之前调用 */

/* IDF 里通常要等 ui_font_init() 跑完再取这个指针 */
lv_pinyin_ime_set_fonts(UI_FONT_TEXT, UI_FONT_TEXT);   /* big(候选/上屏) / small(键面) */
```

两个参数都可以传 `NULL` 表示"这个尺寸继续用自带的"。字体是**建控件时**读的
（和 `lv_pinyin_ime_set_row_height()` 一个道理），已经建出来的控件要等下次重建
键盘才会换。

用了这个之后，`generated/lv_font_ime_*.c` 那两套（约 2.4 MB）就成了死重量；
需要省 flash 的话把 `LV_PINYIN_IME_FONT_SRC_*` 换掉即可（记得同时提供字体，
否则会退回 `LV_FONT_DEFAULT`，中文会全是方框）。

分区表、词库、字体的接入细节见 [integration.md](./integration.md)。

---

## 8. 常用配置项

`idf.py menuconfig` → `LVGL Pinyin IME`，常用几个：

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `LV_PINYIN_IME_DEFAULT_MODE` | 26 键 | 默认键盘模式 |
| `LV_PINYIN_IME_DOUBLE_SCHEME` | 小鹤 | 双拼方案（自然码/微软/ABC/紫光/拼音加加） |
| `LV_PINYIN_IME_ASSOC_DEFAULT` | 开 | 联想 |
| `LV_PINYIN_IME_FUZZY_DEFAULT` | 关 | 模糊音 |
| `LV_PINYIN_IME_CAND_PAGE_SIZE` | 8 | 每页候选数（可见候选按键盘宽度平分整行，右侧不留空；翻页仍由候选栏两端箭头负责） || `LV_PINYIN_IME_DICT_SRC` | 分区 | 词库来源：分区 / SD 卡 / 内嵌 |
| `LV_PINYIN_IME_EXT_DICT_ENABLE` | 关 | SD 卡扩展词库 |

完整列表见 [Kconfig](../Kconfig)。

---

## 9. 目录结构

```
include/lvgl_pinyin_ime/   公共 API（LVGL 组件 + 与 LVGL 无关的引擎门面）
src/ui/                    LVGL 组件实现
src/core/                  会话状态机与引擎辅助（不依赖 LVGL，可主机端单测）
src/data/                  词库 / 用户词库 / 字体装载
src/third_party/           libgooglepinyin（S1 阶段加入）
partitions/                分区表（16 MB / 8 MB）
docs/                      文档
examples/k26_k9_demo/      可编译可烧录的示例
tools/                     PC 侧 Python 工具链（S2 阶段加入）
```

---

## 10. 常见问题

**`idf.py` 不是内部或外部命令**
没有 export。先跑 `& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"`，且必须和 `idf.py` 在同一个终端会话里。

**建完 Python 环境还是提示找不到 `idf6.1_py3.12_env`**
确认 `D:\Espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe` 存在；不存在就重跑一次 `install.ps1`。

**首次构建卡在下载 LVGL**
组件管理器要联网拉 `lvgl/lvgl`，拉完会缓存在 `managed_components/`。断网环境下需要手动 vendor。

**`sdkconfig` 里的目标芯片自己变了**
`idf.py set-target xxx` 是唯一正确的设置方式。注意 `sdkconfig`、`sdkconfig.old`、
`build/`、`managed_components/` 都在 `.gitignore` 里，不要提交。

**装 Python 依赖**
禁止直接 `pip install` 到全局。S2 阶段会提供 `tools/setup_env.ps1`，
它用 IDF 自带的 Python 建 `tools/venv` 再装 `requirements.txt`。

**触摸没反应 / 点不到位置上**
先看屏幕上那行 `tap N at (x, y)` 点屏幕会不会变，再看启动时触摸的自检日志，
然后串口敲 `r` / `d`，按第 5.3 节的表逐条对照；确认控制器有响应后，
敲 `c` 走一遍校准，结果会存进 NVS，重启依然有效。

如果之前跑过旧版本的固件，NVS 里可能留着按旧读取路径算出来的标定，
驱动会自动识别版本不符并忽略（串口有对应警告），敲 `n` 也能手动清掉。

**文字看着发虚、边缘有彩色毛刺**
先确认第 6.1 节的 `LV_COLOR_FORMAT_RGB565_SWAPPED` 在生效（颜色字节序错了会让
抗锯齿的文字边缘串色）。若颜色正常但字仍偏小，那是 LVGL 内置 14px 字体的正常观感；
本项目的 16/20px 中文点阵字体在 S2 阶段生成。

**串口敲命令没反应**
命令是**单字符、不用回车**。用 `idf.py monitor` 时确保窗口有焦点；
若用别的串口工具，注意别开流控（本项目 8N1、无流控）。

**进入校准时崩溃，报 `A stack overflow in task main`**
LVGL 跑在 `app_main` 里，一整屏渲染 + 建控件比默认的 3584 字节栈要得多。
`sdkconfig.defaults` 已经把 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 设成 12288。

注意 `sdkconfig`（生成的）里已有的值**优先于** `sdkconfig.defaults`：
只改 `.defaults` 不会生效，要么在 `menuconfig` 里改，要么删掉 `sdkconfig` 让它重新生成。

**改完 sdkconfig.defaults 没反应**
同上，`sdkconfig` 是生成物且已存在时其中的值优先。删掉 `sdkconfig` 与
`sdkconfig.old` 再 `idf.py build` 即可重新按 defaults 生成（两个文件都在 `.gitignore` 里）。

---

## 11. 文档索引

| 文档 | 内容 |
| --- | --- |
| [usage.md](./usage.md) | 本文：环境、接线、编译烧录、触摸校准 |
| [keymap.md](./keymap.md) | 键位与布局规范（26 键 / 九键 / P1–P3 面板） |
| [architecture.md](./architecture.md) | 分层架构、数据流、libgooglepinyin 移植补丁 |
| [integration.md](./integration.md) | 组件引入、分区表、词库、字体、SD 扩展 |
| [dict_format.md](./dict_format.md) | 扩展词库 / 联想 / 字体镜像的二进制格式 |