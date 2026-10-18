# 词库生成指南

本文只讲**系统词库** `data/dict/dict_pinyin.dat` 怎么生成、怎么改、怎么验证。

- 二进制格式规范：[dict_format.md](./dict_format.md) 第 3 节
- 装载方式 / 分区 / SD 卡：[integration.md](./integration.md) 第 4 节
- 首次检出的整套生成顺序：[usage.md](./usage.md) 第 3 节

---

## 0. 最短路径

```powershell
cd <仓库根>
python tools\gen_dict.py
```

产物 `data/dict/dict_pinyin.dat`（实测 1 068 442 B）。

> `gen_dict.py` 只用 Python 标准库，**系统 Python 即可**，不需要 `tools/venv`——
> venv 里的 fonttools / pypinyin 是 `gen_charset.py`、`gen_font.py` 用的。
> 词库与字体互不依赖，但首次检出后两者都要生成，否则构建缺文件。

---

## 1. 词库为什么是"现场生成"的

不下载上游预编译的 `dict_pinyin.dat`。原因是上游用 `sizeof(size_t)` 写盘：
在 64 位主机上生成的文件按 8 字节字长布局，而 32 位 ESP32 按 4 字节读，
解析出来全是垃圾，引擎只会报一句"打不开词库"。

所以仓库内置原料，用固件**同一份引擎代码**在主机上现编：

| 角色 | 位置 |
| --- | --- |
| 原始词表 | `data/dict/rawdict_utf16_65105_freq.txt` |
| 允许输出的汉字集 | `data/dict/valid_utf16.txt` |
| 构建器入口 | `src/third_party/libgooglepinyin/tools/pinyinime_dictbuilder.cpp` |
| 构建器 / 运行时共用源码 | `src/third_party/libgooglepinyin/src/` |

`gen_dict.py` 先把这些源码编成一个主机端小程序 `generated/dictbuilder[.exe]`，
再让它把词表编成词库。

磁盘格式已由仓库内的补丁钉死，**与主机字宽无关**：

- `include/dictio.h`（P9）：序列化的计数一律 4 字节
- `include/dictdef.h`（P11）：LE0 节点一律 16 字节

---

## 2. 生成

### 2.1 前置条件

| 需要 | 说明 |
| --- | --- |
| Python 3.8+ | 只用标准库 |
| C++ 编译器 | `g++` / `c++` / `clang++` 任一在 PATH 上；Windows 用 MinGW-w64 可（实测 14.2.0） |

不需要 ESP-IDF、不需要联网、不需要 Node.js。

### 2.2 命令

```powershell
python tools\gen_dict.py
```

首次运行会自动编译构建器（十几秒），之后直接复用。实际输出：

```
building dict_pinyin.dat from rawdict_utf16_65105_freq.txt ...
read successfully, lemma num: 65101
spelling tree construct successfully.
------------STAT INFO-------------
...
Build dictionary successfully.
Save dictionary successfully.
wrote ...\data\dict\dict_pinyin.dat (1068442 bytes)
```

词库是可复现的：同一份词表 + 同一份源码，产出逐字节相同。本仓库当前的校验值：
1 068 442 B，SHA-256 `5722707837C94A773B7A833075E46BE1F746FADCCD262BDF156D3E4D4827E02D`
（MinGW-w64 g++ 14.2.0 实测）。

### 2.3 选项

| 选项 | 作用 |
| --- | --- |
| `--rebuild-tool` | 强制重新编译 `generated/dictbuilder` |
| `--cc <路径>` | 指定 C++ 编译器（默认从 PATH 找） |
| `--raw <文件>` | 换一份原始词表 |
| `--valid <文件>` | 换一份汉字集 |
| `--output <文件>` | 换输出路径 |

### 2.4 产物

| 文件 | 说明 |
| --- | --- |
| `data/dict/dict_pinyin.dat` | 系统词库，实测 1 068 442 B（约 1.02 MiB） |
| `generated/dictbuilder.exe` | 主机端构建器，可随手删（下次自动重建） |

### 2.5 校验

先确认字长：

```powershell
python tools\check_dict.py data\dict\dict_pinyin.dat
```

期望结尾是：

```
result: built for a 32-bit reader (4-byte words).
        ESP32 (32-bit) can read it as is.
```

因为磁盘字长已经钉成 4 字节，**用本仓库的构建器生成的词库都应该报这一条**。若报
8-byte，说明这个文件不是本仓库的构建器产出的（比如从上游拿来的预编译词库），
删掉重新生成即可。

再用固件同一份代码把它真正读起来：

```powershell
cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
cmake --build build/host_core_test
.\build\host_core_test\host_core_test.exe data/dict/dict_pinyin.dat
```

---

## 3. 原始词表格式

### 3.1 编码

首字节实测：

```
ff fe | 25 9f | 20 00 | 30 00 2e 00 ... | 0a 00
 BOM  |  鼥   |  空格 |   0.750684...    |  LF
```

- UTF-16 **LE**，**必须**带 BOM（`FF FE`）
- 字段之间是**一个半角空格**，不是 Tab
- 行尾是 **LF**，没有 CR
- 每行一条词：`词条 词频 标记 音节1 [音节2 ...]`

### 3.2 字段

| 字段 | 说明 |
| --- | --- |
| 词条 | 1–4 个汉字 |
| 词频 | 浮点。越大候选越靠前；多字词必须 ≥ 60 |
| 标记 | 上游的 GBK 标志位。**给了 `valid_utf16.txt` 时该字段被忽略**，判据改成"字是否在汉字集里" |
| 音节 | 每个汉字一个，空格分隔，小写、不带声调 |

自带词表共 65105 行，最终入库 65101 条。

### 3.3 哪些行会被丢掉、哪些会让构建直接失败

下面每一条都在本仓库实测过，区别很重要：**有的只是丢一行，有的会让整个构建失败。**

| 情况 | 结果 |
| --- | --- |
| **空行**（中间或末尾都算） | **整个构建失败** |
| 音节数**少于**词条字数 | **整个构建失败** |
| 单个音节超过 6 个字符 | **断言失败**，构建器直接崩 |
| 词表只放很少几条（实测 5 条） | **断言失败**，构建器直接崩 |
| 词条超过 4 个字 | 静默丢掉该行 |
| 多字词（≥2 字）词频 < 60 | 静默丢掉该行 |
| 词条含 `valid_utf16.txt` 之外的字 | 静默丢掉该行 |
| 行尾多出多余字段 | 静默丢掉该行 |
| 音节不是标准拼音（如 `zzz`） | **照常接受**，成为一条可用拼写 |

最后一条意味着词表里可以塞自定义编码（缩写之类），引擎不拦。
前四条是"手滑"的典型后果，所以 3.4 的对账很值得做。

### 3.4 用 lemma num 对账

构建器打印的 `lemma num` 就是真正进库的条数：

```
词表行数 − lemma num = 被静默丢掉的行数
```

自带词表是 65105 行 / 65101 条，差 4。自己加完词后这个差值应只按预期变化；
对不上就说明有行被丢了，回去对照 3.3 的表。

---

## 4. 加入自己的词

推荐做法：在仓库自带词表**尾部追加**，然后整体重新生成。
（这也是目前唯一能让输入法"记住"新词的办法，见 4.3。）

### 4.1 追加

词表是 UTF-16，手改容易破坏编码，用脚本追加最稳：

```python
# 在仓库根执行；只用标准库
import pathlib

raw = pathlib.Path("data/dict/rawdict_utf16_65105_freq.txt")
text = raw.read_bytes().decode("utf-16-le")
if text[0] == "\ufeff":
    text = text[1:]          # BOM 单独处理，别写到中间去

my_words = """
测试 500 0 ce shi
你好 900 0 ni hao
"""

raw.write_bytes("\ufeff".encode("utf-16-le") +
                (text.rstrip("\n") + "\n" + my_words.strip() + "\n").encode("utf-16-le"))
```

每行都要满足 3.2 / 3.3 的约束：**音节数与字数一致、多字词频 ≥ 60、字都在
`valid_utf16.txt` 里、不留空行**（`strip()` 已经帮你挡掉行尾空行）。

然后重新生成：

```powershell
python tools\gen_dict.py
```

实测追加「测试」「你好」两行后，`lemma num` 从 65101 变成 65103，
词库从 1 068 442 B 变成 1 068 458 B（约 8 B / 条）。

### 4.2 新字能不能显示：字体覆盖（最常见的坑）

**编得进词库，不等于画得出来。** 但这一点现在默认已经处理好了：`gen_charset.py`
生成的字表（17 265 字）就是 GB2312 + UI + ASCII + **`valid_utf16.txt`** +
**`rawdict_utf16_65105_freq.txt`** + `extra_chars.txt`，引擎能输出的 16 465 个汉字
一个不缺。改了词表、加了新字之后，**重跑一次字体生成**即可：

```powershell
.\tools\venv\Scripts\python.exe tools\gen_charset.py   # 重扫词表，刷新字表
type generated\font_coverage_report.txt                   # 看还缺什么（正常是 0）
.\tools\venv\Scripts\python.exe tools\gen_font.py      # 重编字体（要几分钟）
```

想再塞进源字体没有、词库也没有的字符（符号、圈号、希腊字母之类），直接写进
`data\dict\extra_chars.txt` —— 它会被并进字表，不用改代码。

体积代价：字表从 7 551 字涨到 17 265 字后，两套字体从约 1.1 MB 涨到约 3.2 MB，
且必须打开 `CONFIG_LV_FONT_FMT_TXT_LARGE`（20 px 字体的位图超过 1 MB，20 位偏移
寻址不到）。细节见 [integration.md](./integration.md) 第 5 节。

### 4.3 用户词（打字学词）目前不落盘

引擎目前以"仅内存用户词库"方式打开，上屏记录不会持久化，`usr` 分区也没有接线。
SD 扩展词库（`ime_ext.bin`）同样只有格式约定、尚无实现，见
[dict_format.md](./dict_format.md) 第 1 节。所以**改词表重新生成是唯一途径**。

---

## 5. 让设备用上新词库

| 方案 | 要做什么 |
| --- | --- |
| 内嵌（默认） | 直接重新 `idf.py build`——CMake 会把 `data/dict/dict_pinyin.dat` 链进 app |
| `dict` 分区 | 重新生成后烧分区：`powershell -ExecutionPolicy Bypass -File tools\flash_dict.ps1 -Port COM5 -Dict` |

内嵌由 `CMakeLists.txt` 的 `LV_PINYIN_IME_EMBED_DICT`（默认 ON）控制，
代价是约 1 MB flash；分区存在时它是兜底副本，设成 OFF 可省掉。

`dict` 分区大小 `0x180000` = 1 572 864 B，当前词库占 1 068 442 B，余量约 492 KB
（见 `partitions/ime_16mb.csv`）。加词很多时留意别超。

---

## 6. 故障排查

| 症状 | 原因 / 处理 |
| --- | --- |
| `no C++ compiler found on PATH` | 装 MinGW-w64 / g++，或用 `--cc <路径>` |
| `missing input: ...` | 原始词表或汉字集不在，检查 `data/dict/` |
| `Save dictionary unsuccessfully` | 输出目录不可写，用 `--output` 换到可写目录 |
| `Build dictionary unsuccessfully` | 有空行，或某行音节数少于字数（3.3） |
| 构建器崩溃 + `Assertion failed` | 有音节超过 6 字符，或词表太小（3.3） |
| `warning: the dictionary looks too small` | 词库 < 500 KB，多半词表被换掉了 |
| 设备上候选为空 | 词库没烧也没重建；或文件是 8 字节字长的旧产物（用 `check_dict.py` 确认） |
| 候选里出现方框 | 字体缺字，按 4.2 扩字符集 |

设备启动日志能直接回答前几类问题：

```
I (xxx) ime_dict: system dictionary: /ime/dict_pinyin.dat (1068442 bytes)
I (xxx) ime_dict: self test: read 1068442 of 1068442 bytes from /ime/dict_pinyin.dat
```

---

## 7. 校验清单

| 检查项 | 怎么做 | 期望 |
| --- | --- | --- |
| 词库生成成功 | `python tools\gen_dict.py` | 退出码 0，打印 `Save dictionary successfully.` |
| 条数对得上 | 看 `lemma num` | 行数 − lemma num = 预期的丢弃数 |
| 字长正确 | `python tools\check_dict.py ...` | `4-byte words ... ESP32 can read it as is` |
| 引擎能读 | `host_core_test.exe data/dict/dict_pinyin.dat` | 全部通过 |
| 新字能显示 | `generated/font_coverage_report.txt` | `engine characters this font lacks: 0` |
| 设备上生效 | 启动日志的 `system dictionary` / `self test` | 字节数与本地文件一致 |
