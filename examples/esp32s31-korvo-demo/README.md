# esp32s31-korvo-demo

把 `lvgl_pinyin_ime` 跑在 **ESP32-S31-Korvo-1 V1.1** 上的最小示例：4.3 英寸
800 × 480 RGB 屏 + GT1151 电容触摸 + 中文拼音键盘。

它是 [k26_k9_demo](../k26_k9_demo) 的姊妹篇：同一个输入法组件，换了一块板、换了一套
显示与触摸驱动。想先了解组件本身，看仓库根目录的 [README](../../README_zh-cn.md)；
想接进自己的工程，看 [docs/integration.md](../../docs/integration.md)。

## 1. 硬件

| 部件 | 型号 / 参数 |
| --- | --- |
| 主板 | ESP32-S31-Korvo-1 V1.1，ESP32-S31-WROOM-3（16 MB flash + 16 MB PSRAM） |
| 屏幕子板 | ESP32-S3-LCD-EV-Board-SUB3，4.3 英寸，800 × 480，RGB565，驱动 IC ST7262E43 |
| 触摸 | GT1151（I2C，地址 0x14），与屏幕贴合，无需校准 |
| 音频 | ES8389 编解码器 + 双麦阵列 + 双路 NS4150B 功放（本示例未使用） |

屏幕子板**必须插上**：Korvo-1 主板本身不带屏。官方用户指南：
<https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s31/esp32-s31-korvo-1/user_guide.html>

### 1.1 引脚

与乐鑫官方 BSP（`espressif/esp32_s31_korvo_1`）一致，改板时只改
[main/board_display.c](main/board_display.c) 顶部这几组宏：

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| RGB DB0 .. DB15 | 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 33, 34, 35, 36 | 16 位并口数据 |
| LCD_PCLK | 40 | 像素时钟，18 MHz |
| LCD_DE | 43 | 数据使能 |
| LCD_HSYNC / LCD_VSYNC | 44 / 45 | 行 / 场同步 |
| LCD_DISP | 38 | 显示使能（与 LCD_CS、Boot Mode 0、摄像头 GM_FK 复用） |
| I2C SDA / SCL | 0 / 1 | 触摸；与 ES8389 共用同一条总线 |

板子没有 LCD 复位脚、也没有背光控制脚：上电后背光常亮，DISP 是唯一的控制线。
因此本示例**不做**背光调节，也不需要触摸校准。

## 2. 和 k26_k9_demo 的差别

| | k26_k9_demo | 本示例 |
| --- | --- | --- |
| 屏幕 | ST7789，SPI，320 × 240 | ST7262E43，RGB 并口，800 × 480 |
| 触摸 | XPT2046 电阻屏，需校准 | GT1151 电容屏，不用校准 |
| LVGL 驱动 | 自己写 flush 回调 + `esp_lcd_panel_draw_bitmap` | `esp_lvgl_port`（自带 LVGL 任务、互斥锁、RGB flush） |
| 键盘高度 | 默认行高 | `lv_pinyin_ime_set_fit_height(220)` 固定成 220 px |
| 主循环 | `app_main` 里跑 `lv_timer_handler()` | `app_main` 只建界面，渲染交给 port 的 task |

依赖由组件管理器自动拉取（见 [main/idf_component.yml](main/idf_component.yml)）：
LVGL（`^9.5`，当前解析到 9.6）、`espressif/esp_lvgl_port`、`espressif/esp_lcd_touch_gt1151`。

> 为什么不用整套 `espressif/esp32_s31_korvo_1` BSP？BSP 会把 `esp_codec_dev`、
> `esp_video`（摄像头）、`button`、`led_indicator`、`usb` 一起拉进来，对一个
> 键盘 demo 来说太重了。本示例只借用了 BSP 的**引脚与 RGB timing**（见
> `board_display.c` 顶部注释），依赖保持在"一个显示 + 一个触摸"。
> 需要音频/摄像头时再换 BSP 即可，两者可以共存。

## 3. 编译与烧录

前提：ESP-IDF **6.1**（ESP32-S31 在 6.1 里是 **preview 目标**）、组件管理器能联网。
词库与字体要先在仓库根目录生成一次（`generated/lv_font_ime_*.c`、`data/dict/dict_pinyin.dat`），
步骤见 [docs/usage.md](../../docs/usage.md) 第 2、3 节。

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"   # 换成你的 IDF 路径
cd examples\esp32s31-korvo-demo

# ★ S31 是 preview 目标，set-target 必须带 --preview，否则会报 unknown target
idf.py --preview set-target esp32s31

idf.py build
idf.py -p COM5 flash monitor                      # Ctrl + ] 退出
```

- 第一次构建会联网把 `lvgl`、`esp_lvgl_port`、`esp_lcd_touch_gt1151` 拉到
  `managed_components/`
- 换芯片后要重跑 `idf.py --preview set-target <target>`，否则 `sdkconfig` 里的
  target 还是旧的
- **改过 `sdkconfig.defaults` 之后要删掉同目录的 `sdkconfig`**（或重跑
  `idf.py --preview set-target esp32s31`）才会生效：ESP-IDF 只在生成 `sdkconfig`
  的那一刻读 `sdkconfig.defaults`，之后就以 `sdkconfig` 为准了

### 3.1 分区、词库与字库

[sdkconfig.defaults](sdkconfig.defaults) 用的是仓库统一的 16 MB 布局
[`partitions/ime_16mb.csv`](../../partitions/ime_16mb.csv)：`dict` 3 MB、`font` 3 MB、
`usr` 1 MB、`factory` 8 MB。

**词库**来自编进应用的副本（`CONFIG_LV_PINYIN_IME_DICT_SRC_EMBEDDED`）。示例刻意这么
配：`idf.py flash` 只写引导程序、分区表和应用，不写 `dict` 分区；若保持"从分区读"
的默认值，每次启动都会先报

```
E ime_dict: dict partition is not a dictionary (1142170 bytes, starts with FF FF FF FF)
```

再回退到内嵌副本 —— 能用，但日志难看还多一次挂载。要改用分区（能省约 1 MB 应用体积）
就把这个选项换回 `LV_PINYIN_IME_DICT_SRC_PARTITION`，并跑一次
[`tools/flash_dict.ps1`](../../tools/flash_dict.ps1)。

**字库**默认也编进应用（`FONT_SRC_EMBEDDED`）：字符集 = GB2312 + ASCII（`0x20`–`0x7E`）
+ `data/dict/` 下三个词表里出现的**全部**字符 + `extra_chars.txt`，合计约 1.7 万字；
所以键盘上的拉丁字母、数字、拼音串和中英文候选都有字形，不会画成方框。
字库生成物不入库，改完字表要重新生成并重编：

```powershell
cd <仓库根>
.\tools\venv\Scripts\python.exe tools\gen_charset.py   # 字表 -> generated/charset.txt
.\tools\venv\Scripts\python.exe tools\gen_font.py      # 字表 -> generated/lv_font_ime_*.c
```

想省 flash 可以把 `LV_PINYIN_IME_FONT_SRC` 切成 `PARTITION`，烧
`generated/font_partition.bin` 到 `font` 分区，并打开 `CONFIG_LV_USE_FS_MEMFS`
（本示例默认没开）。

### 3.2 正常的启动日志

```
I (xxx) korvo_display: RGB panel up: 800x480, 1 frame buffer(s), 18 MHz PCLK
I (xxx) korvo_display: GT1151 touch ready
I (xxx) korvo_demo: display: registered with LVGL
I (xxx) korvo_demo: heap: internal ......  B, PSRAM ....... B
I (xxx) korvo_demo: keyboard: created, mode=0 lang=0
```

`lv_pinyin_ime_dump()` 还会打印控件树与每一行按键的坐标，屏幕没画出来时先看它。

## 4. 屏幕 / 触摸不对怎么办

| 现象 | 原因 | 改哪里 |
| --- | --- | --- |
| 全白或满屏花点 | RGB timing / 数据脚不对，或 PSRAM 没开 | 检查 `sdkconfig.defaults.esp32s31` 的 `CONFIG_SPIRAM=y`；对照 `PIN_LCD_*` 与数据脚顺序 |
| 画面像负片 | 这块屏不需要反色，但批次可能不同 | [board_display.c](main/board_display.c) 里 `esp_lcd_panel_invert_color(panel, true)` |
| 画面上下颠倒 / 左右镜像 | 子板安装方向 | `esp_lcd_panel_mirror(panel, x, y)` 的两个参数 |
| 触摸点得到但位置反了 | 触摸坐标轴与屏幕不一致 | `tp_cfg.flags` 里的 `swap_xy` / `mirror_x` / `mirror_y` |
| 触摸完全没反应 | I2C 没通，或 GT1151 没供电 | 看启动日志里有没有 `touch init failed`；GT1151 在 0x14 |
| 打字时画面有撕裂 | 单帧缓冲 + 部分刷新 | 见下一节 |
| 刷新很慢 | LVGL 缓冲在 PSRAM，且每次只刷 100 行 | 调大 `LCD_DRAW_BUF_LINES`（代价是内存） |

### 4.1 关于撕裂与帧缓冲

本示例用**一块** RGB 帧缓冲（在 PSRAM）+ LVGL 双缓冲部分刷新：改动区域由 CPU 拷进
帧缓冲，屏幕同时还在扫描它，所以整屏动画时能看到轻微撕裂，换来的是打字时只重画
改动区域。

想要完全无撕裂，把 `board_display.c` 里 `LCD_NUM_FB` 改成 `2`，并在
`lvgl_port_display_rgb_cfg_t` 里把 `avoid_tearing` 打开 —— 此时 esp_lvgl_port 会把
两块帧缓冲直接当 LVGL 画布（全屏刷新），不再有拷贝，但每帧要重画 800 × 480。
RGB 的两个缓冲合计 1.5 MB，仍然在 16 MB PSRAM 里。

## 5. 语音输入（本示例未接）

Korvo-1 有双麦阵列和 ES8389，做语音输入是合适的，但需要额外的音频栈：

1. 用 `esp_codec_dev` + ES8389 驱动（或直接用 BSP 的
   `bsp_audio_codec_microphone_init()`）按 16 kHz / 16 bit / 单声道读 PCM；
2. 用 `esp_websocket_client` 连讯飞（或你自己的服务）；
3. 实现 `lv_pinyin_ime_voice_host_t` 的四个回调，调用
   `lv_pinyin_ime_voice_attach()` / `lv_pinyin_ime_voice_set_config()`。

组件这一侧只要 `CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y` 就能用，协议细节见
[docs/voice.md](../../docs/voice.md)；[k26_k9_demo/main/mic_i2s.c](../k26_k9_demo/main/mic_i2s.c)
是一份可照抄的"麦克风 → 组件"参考，只是那边用的是 PDM 麦克风。

## 6. 文件

| 文件 | 作用 |
| --- | --- |
| [main/main.c](main/main.c) | 界面：标题、状态行、输入框、三个工具栏按钮、键盘 |
| [main/board_display.c](main/board_display.c) | RGB 屏 + GT1151 + esp_lvgl_port 的板级初始化 |
| [main/board_display.h](main/board_display.h) | 上面对外暴露的三个函数 |
| [main/idf_component.yml](main/idf_component.yml) | LVGL / esp_lvgl_port / esp_lcd_touch_gt1151 |
| [sdkconfig.defaults](sdkconfig.defaults) | 16 MB flash、分区表、LVGL 配置 |
| [sdkconfig.defaults.esp32s31](sdkconfig.defaults.esp32s31) | PSRAM（8 线模式） |
