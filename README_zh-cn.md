# lvgl_pinyin_ime

面向 **ESP-IDF 6.1** + **LVGL 9.5** 的中文拼音输入法组件。

<table>
  <tr>
    <td align="center"><img src="https://github.com/user-attachments/assets/5c1e9c02-6adc-4e11-a848-4a5f6544f932" width="200"></td>
    <td align="center"><img src="https://github.com/user-attachments/assets/ca59b750-8422-4a81-83e6-ea33bf7a3dd9" width="200"></td>
    <td align="center"><img src="https://github.com/user-attachments/assets/66cafd9e-4c00-440b-9213-55d1b87d6ca8" width="200"></td>
  </tr>
  <tr>
    <td align="center">运行在嵌入式 Linux</td>
    <td align="center">运行在 ESP32S3 26 键</td>
    <td align="center">运行在 ESP32S3 T9 键</td>
  </tr>
</table>

> [!WARNING]
> 该仓库为 AI 生成。

[English](README.md) | **简体中文**

`lvgl_pinyin_ime` 是一个可直接被 ESP-IDF 工程以组件形式引入的 LVGL 控件：给出一个
`lv_textarea`，它负责键盘、候选、上屏与拼音解码。

- 引擎：[libgooglepinyin](https://salsa.debian.org/input-method-team/libgooglepinyin)（内置，Apache-2.0）
- 键盘：26 键全键盘 + 9 键（T9），外加共用的数字/符号面板
- 输入：全拼、词组、整句，带候选翻页、音节分隔符 `'`、中英切换
- 词库：flash 分区 / SD 卡 / 内嵌三种来源，`dict` 分区缺失时自动回退到内嵌副本
- 字体：默认以 C 数组内嵌进固件，无需烧录分区；也可改用 `font` 分区
- 语音输入（可选）：讯飞流式听写，组件跑协议，应用提供 socket 与麦克风
- 目标平台：ESP32-S3（主要）、ESP32-P4；ESP32-S31 为 preview 目标
- 参考板：ST7789（SPI，320 x 240 横屏）+ XPT2046 触摸
- 另有 **ESP32-S31-Korvo-1** 的示例（800 x 480 RGB 屏 + GT1151 触摸）：
  [examples/esp32s31-korvo-demo](examples/esp32s31-korvo-demo)

**不知道从哪开始？** 只是想编译、烧录、看效果，直接看下面的[快速开始](#快速开始跑示例)；
要把它接进你自己的工程，看[接入你自己的工程](#接入你自己的工程)。

## Todo List:

- [x] 添加语音输入支持。
- [x] 添加词频排序。(Beta)
- [ ] 添加词语联想。
- [ ] 添加常用词语记忆。

## 快速开始（跑示例）

四个步骤：准备环境 → 生成词库与字体 → 编译 → 烧录。

### 0. 前置条件

| 需要什么 | 说明 |
| --- | --- |
| ESP-IDF 6.1 | 已 `export` 到当前 PowerShell 会话 |
| Python 3.8+ | 生成词库与字体，一律在 `tools/venv` 里，不装全局 |
| Node.js LTS | 仅生成字体时用到（`lv_font_conv` 依赖），`winget install OpenJS.NodeJS.LTS` |

### 1. export IDF 环境

`idf.py`、`esptool` 都由它注入当前会话；每个新终端都要执行一次：

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"   # 换成你的 IDF 安装路径
```

### 2. 生成词库与字体（首次必须）

仓库根目录的**生成物不入库**（`data/` 里的词表除外），所以首次检出后必须先生成，
否则构建会因缺少 `generated/lv_font_ime_*.c` 而直接报错。

```powershell
# 建独立 venv 并装依赖（只需一次）
powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1

# 字表 -> 字体 C 数组；词表 -> 词库
.\tools\venv\Scripts\python.exe tools\gen_charset.py
.\tools\venv\Scripts\python.exe tools\gen_font.py
python tools\gen_dict.py
```

词库生成的细节——词表格式、怎么加自己的词、生成失败怎么查——见
[docs/dict_build.md](docs/dict_build.md)。

### 3. 编译

**仓库根目录是组件，不是工程** —— `idf.py` 必须在工程目录里运行，否则 CMake 会把
仓库根当作工程来配置并以主机编译器报错告终。示例工程在 `examples/k26_k9_demo`：

```powershell
cd examples\k26_k9_demo
idf.py set-target esp32s3        # 只需做一次；换芯片时再改
idf.py build
```

首次构建会联网把 LVGL 9.5 拉到 `managed_components/`。

板子是 **ESP32-S31-Korvo-1** 的话，把第 3 步的目录换成
`examples\esp32s31-korvo-demo`，并且目标要带 `--preview`（S31 在 IDF 6.1 里是
preview 目标）：

```powershell
cd examples\esp32s31-korvo-demo
idf.py --preview set-target esp32s31
idf.py build
```

屏幕/触摸/语音的说明见该目录的 [README](examples/esp32s31-korvo-demo/README.md)。

### 4. 烧录并观察

```powershell
idf.py -p COM5 flash monitor     # COM5 换成你的串口；Ctrl + ] 退出
```

`idf.py flash` 会写入引导程序、分区表与应用程序；应用内已带一份词库兜底副本，
因此**没有单独烧录 `dict` 分区也能直接工作**。

示例启动时会打印控件树（`lv_pinyin_ime_dump`），引擎或字体缺失时也会报错，
从串口输出即可回答「键盘为什么没出现」。屏幕接线、触摸校准、显示异常排查见
[docs/usage.md](docs/usage.md)。

## 接入你自己的工程

### 1. 声明依赖

组件通过 ESP-IDF 组件管理器引入，源就是本项目的 GitHub 仓库。`main/idf_component.yml`：

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

组件依赖仓库内自带的 `libgooglepinyin`，而且 `generated/`（词库与字体）不入库 ——
所以还差两步，**只写 `idf_component.yml` 是构建不起来的**：

1. 工程根 `CMakeLists.txt` 里把
   `managed_components/lvgl_pinyin_ime/src/third_party/libgooglepinyin` 加进
   `EXTRA_COMPONENT_DIRS`（ESP-IDF 不扫描组件自己的子目录）；
2. 在拉取下来的 `managed_components/lvgl_pinyin_ime/` 里生成一次词库与字体。

两条命令、以及「想反复改字表就用 `override_path` 指本地 clone」的建议，见
[docs/integration.md](docs/integration.md) 第 2.2 节。

### 2. 让 main 依赖它

`main/CMakeLists.txt`：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl lvgl_pinyin_ime
)
```

### 3. 写代码

```c
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

lv_obj_t *ta  = lv_textarea_create(lv_screen_active());
lv_obj_t *ime = lv_pinyin_ime_create(lv_screen_active());

lv_pinyin_ime_attach(ime, ta);                        /* 上屏目标 */
lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K26);  /* 26 键 / 九键 */
lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_CN);   /* 中文 / 英文 */

/* 点 ✓ 且拼音缓冲为空时抛出，应用据此收起键盘 */
lv_obj_add_event_cb(ime, on_ime_ready, lv_pinyin_ime_event_ready(), NULL);
```

回调：

```c
static void on_ime_ready(lv_event_t *e)
{
    lv_obj_add_flag(ime, LV_OBJ_FLAG_HIDDEN);   /* 收起键盘 */
}
```

这样就够用了。以下三点在建控件**之前**调用，属于全局状态，建完再改不会重排：

- `lv_pinyin_ime_set_fit_height(px)` —— 按目标高度自适应行高，键盘占屏幕一半时最省心
- `lv_pinyin_ime_set_row_height(px)` —— 需要固定行高时使用
- `lv_pinyin_ime_set_fonts(big, small)` —— 用应用自己的中文字体，字更全，避免缺字方框

分区表、词库来源、字体两种模式、SD 扩展词库等接入细节见
[docs/integration.md](docs/integration.md)；完整 API 见
[include/lvgl_pinyin_ime/](include/lvgl_pinyin_ime/)。

## 语音输入（可选）

键盘上多一个语音键，按住或点一下就能把说的话变成文字进输入框；识别在云端做，用的是
讯飞的[语音听写（流式版）WebAPI](https://www.xfyun.cn/doc/asr/voicedictation/API.html)。
用 `CONFIG_LV_PINYIN_IME_VOICE_ENABLE` 打开，密钥在 menuconfig 的
`LVGL Pinyin IME → Voice input` 里填。

组件**刻意不碰网络、也不碰 I2S**：应用提供四个回调（`ws_open` / `ws_send_text` /
`ws_close` / `audio_read`），所以不管用哪家云、哪个 WebSocket 库、哪款音频 codec，
组件的依赖表始终只有 `lvgl` + `libgooglepinyin`。组件负责的是协议本身：HMAC-SHA256
请求签名、40 ms / 1280 字节的音频分帧、返回结果解析、动态修正拼接、会话状态机。

这样切分还有个好处：协议层能在 PC 上测完 —— 签名、分帧、解析整条链都拿讯飞文档里的
样例逐字节校验，不需要网络也不需要板子。接入步骤见 [docs/voice.md](docs/voice.md)。

## 功能与现状

| 能力 | 状态 |
| --- | --- |
| 26 键全键盘、9 键（T9）、数字/符号面板（P1/P2/P3） | 可用 |
| 全拼、词组、整句；候选翻页；音节分隔符 `'`；中英切换 | 可用 |
| 词库三种来源（分区 / SD / 内嵌）+ 分区缺失回退 | 可用 |
| 字体内嵌（默认）或 `font` 分区 | 可用 |
| 语音输入（讯飞流式听写） | 可用（默认关，需自备 Wi-Fi 与麦克风） |
| 双拼、模糊音、联想、用户词频学习、SD 扩展词库 | 规划中，尚未落地 |

> 「规划中」的能力在 `Kconfig` 里已有条目、UI 里也预留了入口（例如分词键），但底层
> 模块尚未实现：双拼键位映射、模糊音变体、联想（bigram）与用户词典落盘都还没有代码。
> 引擎门面已封装了上游的联想接口 `ime_engine_predict()`，但 UI 尚未调用。

## 验证（不需要硬件）

```powershell
# 引擎 / T9 / 字典树 / 哈希表的主机单测
cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
cmake --build build/host_core_test
.\build\host_core_test\host_core_test.exe data/dict/dict_pinyin.dat

# 语音协议层主机测试：签名、分帧、返回解析、动态修正（不联网）
gcc -std=c11 -Wall -Wextra -Werror -Isrc `
  test_apps/host_voice_test/main.c `
  src/voice/ime_voice_proto.c src/voice/ime_voice_crypto.c `
  -o build/host_voice_test/host_voice_test.exe
.\build\host_voice_test\host_voice_test.exe

# 组件编译检查（针对某个目标，无需硬件）
cd test_apps\idf_component_build
idf.py set-target esp32s3 ; idf.py build
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/usage.md](docs/usage.md) | 环境、接线、编译烧录、触摸校准、故障排查 |
| [docs/integration.md](docs/integration.md) | 作为组件接入：依赖、分区表、词库、字体 |
| [docs/keymap.md](docs/keymap.md) | 键位布局与逐键行为（26 键 / 九键 / 面板） |
| [docs/architecture.md](docs/architecture.md) | 分层、模块清单、数据流、算法、移植补丁 |
| [docs/voice.md](docs/voice.md) | 语音输入接入与实现要点 |
| [docs/dict_build.md](docs/dict_build.md) | 词库生成：词表格式、加自己的词、校验与排查 |
| [docs/dict_format.md](docs/dict_format.md) | 词库 / 字体镜像的二进制格式 |

## 目录结构

```
include/lvgl_pinyin_ime/  对外 API（LVGL 控件 + 引擎门面 + 语音）
src/ui/                   LVGL 控件实现
src/core/                 会话状态机与引擎辅助（不依赖 LVGL，可主机端单测）
src/data/                 词库 / 字体装载
src/voice/                讯飞协议：签名、分帧、解析、状态机
src/third_party/          内置的 libgooglepinyin
partitions/               分区表（16 MB / 8 MB）
docs/                     上述文档
examples/k26_k9_demo/     可构建示例：ST7789 + XPT2046 + IME 宿主（含语音接线）
tools/                    主机侧 Python 工具链（gen_dict / gen_font ...）与检查脚本
test_apps/                主机单测与针对目标的编译检查
```

## 算法

解码栈分为两层，本项目目前的实现情况如下。

**引擎层（libgooglepinyin，unigram）。** `im_search()` 把拼写串交给 `MatrixSearch`，
后者遍历音节字典树（`spellingtrie.cpp`）、在词条缓存（`lpicache.cpp`）中查找候选，
并用 `NGram::get_uni_psb()` 打分。这是 **unigram** 模型：每个词条带一个独立的词频，
没有跨词上下文。`ngram.h` 只暴露 `get_uni_psb()` 与 `build_unigram()` —— 上游没有
任何 bigram 构建函数，因此单次搜索步内，词 *n* 不影响词 *n+1* 的得分。多音节句子的
分词是在这些 unigram 得分之上用动态规划（`extend_mtrx_nd()`）求解的。

**联想层（本项目，bigram）尚未落地。** 计划中，候选上屏之后由联想层预测下一个词，
2-gram 信息放在这一层，配一张 bigram 哈希表（`ime_hash.c`，该模块本身已实现，但
目前还没有调用方）。上游的联想接口 `im_get_predicts()` 已由 `ime_engine_predict()`
封装，UI 尚未接线。因此**当前实际生效的只有 unigram 引擎这一层**。

所以：与 libpinyin / sunpinyin 比较时，准确的说法是「libgooglepinyin 的引擎仅使用
unigram 打分」，而不是「本项目仅使用 unigram」。

9 键路径自身不带任何打分：`ime_trie.c` 把数字串映射为候选音节，`ime_t9.c` 将其展开，
两者都是查表，随后把拼好的全拼串交给同一个引擎。排序逻辑只有一处 —— 引擎。

## 相关项目

如果你想了解功能更完整的拼音输入法，可以试试 **libpinyin** 或 **sunpinyin**；
两者都受 fcitx 和 ibus 支持。它们使用更高级的算法（2-gram 语言模型）并支持更多功能，
例如双拼、注音和模糊拼音。请注意它们面向桌面环境 —— 依赖 GLib，且 sunpinyin 还依赖
SQLite 承载的语言模型，因此并不适合 ESP32 目标。

如果你需要 libgooglepinyin 之上的额外功能，可以试试 **fcitx-googlepinyin**，
即可获得 fcitx 的快捷短语、虚拟键盘、云拼音和可自定义标点符号。

## 许可证

Apache-2.0。见 [LICENSE](LICENSE)。内置的 libgooglepinyin 保留其自身的
Apache-2.0 许可证与上游署名，位于 `src/third_party/` 下。
