# lvgl_pinyin_ime

面向 **ESP-IDF 6.1** 上 **LVGL 9.5** 的中文拼音输入法（IME）组件。

<!-- <div align="center">
  <img height="1080" alt="IMG_20261003_022916" src="https://github.com/user-attachments/assets/5c1e9c02-6adc-4e11-a848-4a5f6544f932" />
  <p>运行在嵌入式 Linux</p> 
  <img width="1080" height="1920" alt="26-keyboard" src="https://github.com/user-attachments/assets/ca59b750-8422-4a81-83e6-ea33bf7a3dd9" />
  <p>运行在ESP32S3 26键盘</p> 
  <img width="3072" height="4096" alt="T9-keyboard" src="https://github.com/user-attachments/assets/66cafd9e-4c00-440b-9213-55d1b87d6ca8" />
  <p>运行在ESP32S3 T9键盘</p> 
</div> -->

<table>
  <tr>
    <td align="center"><img src="https://github.com/user-attachments/assets/5c1e9c02-6adc-4e11-a848-4a5f6544f932" width="200"></td>
    <td align="center"><img src="https://github.com/user-attachments/assets/ca59b750-8422-4a81-83e6-ea33bf7a3dd9" width="200"></td>
    <td align="center"><img src="https://github.com/user-attachments/assets/66cafd9e-4c00-440b-9213-55d1b87d6ca8" width="200"></td>
  </tr>
  <tr>
    <td align="center">运行在嵌入式 Linux</td>
    <td align="center">运行在ESP32S3 26键盘</td>
    <td align="center">运行在ESP32S3 T9键盘</td>
  </tr>
</table>

> [!WARNING]
> 该仓库为AI生成。

[English](README.md) | **简体中文**

- 引擎：[libgooglepinyin](https://salsa.debian.org/input-method-team/libgooglepinyin)（Apache-2.0）
- 键盘：26 键全键盘 + 9 键（T9），外加共用的数字/符号面板
- 输入：全拼、双拼（六套方案，默认小鹤）、词组、整句
- 特性：候选翻页、音节分隔符 `'`、联想、用户词频学习、模糊音（默认关闭）
- 目标平台：ESP32-S3-N16R8（主要）、ESP32-S31、ESP32-P4
- 参考板：ST7789（SPI，320 x 240 横屏）+ XPT2046 触摸

如果你只想编译、烧录并运行示例，请从 [docs/usage.md](docs/usage.md) 开始。

## 状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| S-1 | 设计文档（`docs/`） | 完成 |
| S0 | 工程骨架、示例应用、三目标构建 | 完成 |
| S1 | libgooglepinyin 移植 + 引擎门面 + 词库装载 | **完成** |
| S2 | Python 工具链（词库/字符集/字体） | **完成**（扩展词库随 S5 落地） |
| S3 | UI：根对象、候选栏、26 键、面板 | **完成** |
| S4 | 9 键：T9 缓冲、字典树、哈希表 | **完成** |
| S5 | 双拼、模糊音、联想、用户词典 | 待做 |
| S6 | 主机单测、文档、三目标回归 | 进行中 |

目前有两条验证路径可用，都不需要硬件：

```powershell
# 引擎、T9、字典树与哈希表的主机单测（64 条断言）
cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
cmake --build build/host_core_test
.\build\host_core_test\host_core_test.exe data/dict/dict_pinyin.dat

# 针对某个目标的组件编译检查
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd test_apps\idf_component_build
idf.py set-target esp32s3 ; idf.py build
```

词库由仓库内自带的上游词表重建，而不是下载：`python tools/gen_dict.py`
（见 [docs/usage.md](docs/usage.md)）。

## 目录结构

```
include/lvgl_pinyin_ime/  对外 API（LVGL 控件 + 不依赖 LVGL 的引擎门面）
src/ui/                    LVGL 控件
src/core/                  会话状态机与引擎辅助模块
src/data/                  词库、用户词典、字体装载
src/third_party/           内置的 libgooglepinyin（S1）
partitions/                分区表（16 MB / 8 MB）
docs/                      usage、keymap、architecture、integration、dict format
examples/k26_k9_demo/      可构建示例：ST7789 + XPT2046 + IME 宿主
tools/                     主机侧 Python 工具链（gen_dict、gen_t9_table）
test_apps/host_core_test/  不依赖 LVGL 模块的主机单测
test_apps/idf_component_build/  针对目标的编译检查，无需硬件
```

## 快速开始

**仓库根目录是组件，不是工程** —— `idf.py` 必须在工程目录下运行，否则 CMake 会把
仓库根目录当作工程来配置，并以主机编译器报错告终。

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd examples\k26_k9_demo          # <- 这里是工程，不是仓库根目录
idf.py set-target esp32s3
idf.py -p COM17 flash monitor
```

首次构建会把 LVGL 9.5 下载到 `managed_components/`。
ESP32-S31 仍是预览目标：`idf.py --preview set-target esp32s31`。

`idf.py flash` 会写入引导程序、分区表和应用程序。
应用程序以 C 数组形式内置中日韩字体（键盘、候选词和已上屏中文都无需额外烧录），
并内置一份 1 MB 的词库兜底副本。可选的 `dict` 和 `font` 分区需单独烧录；只有当你把
`CONFIG_LV_PINYIN_IME_FONT_SRC` 从 `EMBEDDED` 切换为 `PARTITION` 时，字体才需要
`font` 分区：

```powershell
python tools\gen_dict.py          # data/dict/dict_pinyin.dat
python tools\gen_font.py          # generated/lv_font_ime_*.c（内置，默认）
python tools\gen_font.py --format bin   # generated/font_partition.bin（分区模式）
powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM17 -Dict -Font
```

示例会在启动时打印控件树（`lv_pinyin_ime_dump`），引擎缺失时也会报错，
因此从监视器输出即可回答「键盘为什么没出现」。

示例目前会展示完整键盘：带拼音浮窗的候选栏、26 键键盘（外加 P1/P2/P3 符号面板），
以及带候选拼音行的 9 键键盘。按键为圆角方块
（见 `src/ui/lv_pinyin_ime_style.c` 中的 `IME_KEY_RADIUS`）。IME 从 `dict` flash
分区读取词库，并回退到链接进应用程序的副本，因此在任何分区被烧录之前也能工作；
用 `tools/flash_dict.ps1` 烧录 1 MB 词库即可去掉该兜底。

中日韩字体已编译进应用程序，所以刚烧录完的板子上，键盘、候选栏和已上屏文字都能
显示真实字形 —— 不需要字体分区。`tools/gen_font.py` 会把 `lv_font_conv` 输出的
LVGL 8 结构后处理成 LVGL 9 的结构布局；它改了什么、为什么改，见
[docs/integration.md](docs/integration.md) 第 5 节。

## 文档

- [docs/usage.md](docs/usage.md) —— 环境、接线、编译/烧录、触摸校准
- [docs/keymap.md](docs/keymap.md) —— 键位布局与逐键行为
- [docs/architecture.md](docs/architecture.md) —— 分层、数据流、移植补丁
- [docs/integration.md](docs/integration.md) —— 组件接入、分区、字体、SD 词库
- [docs/dict_format.md](docs/dict_format.md) —— 词库与字体镜像的二进制格式

## 算法

解码栈分为两层，两层使用的模型不同。在把本项目与桌面 IME 引擎作比较时，这一点很
关键。

**引擎层（libgooglepinyin，unigram）。** `im_search()` 把拼写串交给 `MatrixSearch`，
后者遍历音节字典树（`spellingtrie.cpp`）、在词条缓存（`lpicache.cpp`）中查找候选，
并用 `NGram::get_uni_psb()` 打分。这是 **unigram** 模型：每个词条带一个独立的词频，
没有跨词上下文。`ngram.h` 只暴露 `get_uni_psb()` 与 `build_unigram()` —— 上游没有
任何 bigram 构建函数，因此单次搜索步内，词 *n* 不影响词 *n+1* 的得分。多音节句子的
分词是在这些 unigram 得分之上用动态规划（`extend_mtrx_nd()`）求解的。

**联想层（本项目，bigram）。** 候选上屏之后，由 `ime_assoc` 预测下一个词。2-gram
信息就在这一层，而且它是本项目自己实现的，不来自上游：一张 bigram 哈希表
（`ime_hash.c`），外加首字联想和用户自身条目的近期列表。其数据来自
`ime_bigram.bin`，由 `tools/gen_bigram.py` 离线生成；若未提供语料，该文件不会生成，
联想会自动降级为首字联想加用户近期词。**该层属于 S5 —— 尚未落地。**

所以：*引擎* 是纯 unigram 的，这是继承自 2009 年上游的真实短板；而 *本项目* 在其上
另加了一层 bigram 联想。与 libpinyin 或 sunpinyin 作比较时，准确的说法应是
「libgooglepinyin 的引擎仅使用 unigram 打分」，而不是「本项目仅使用 unigram」。

9 键路径自身不带任何打分：`ime_trie.c` 把数字串映射为候选音节，`ime_t9.c` 将其展开，
两者都是查表，随后把拼好的全拼串交给同一个引擎。排序逻辑只有一处 —— 引擎加联想层。

## 相关项目

如果你想了解功能更完整的拼音输入法，可以试试 **libpinyin** 或 **sunpinyin**；
两者都受 fcitx 和 ibus 支持。它们使用更高级的算法（2-gram 语言模型，而
libgooglepinyin 的引擎只使用 unigram），并支持更多功能，例如双拼、注音和模糊拼音。
请注意它们面向桌面环境 —— 依赖 GLib，且 sunpinyin 还依赖 SQLite 承载的语言模型，
因此并不适合 ESP32 目标。

如果你需要 libgooglepinyin 之上的额外功能，可以试试 **fcitx-googlepinyin**，
即可免费获得 fcitx 的快捷短语、虚拟键盘、云拼音和可自定义标点符号。

## 许可证

Apache-2.0。见 [LICENSE](LICENSE)。内置的 libgooglepinyin 保留其自身的
Apache-2.0 许可证与上游署名，位于 `src/third_party/` 下。
