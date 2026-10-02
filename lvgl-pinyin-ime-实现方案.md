# LVGL 中文拼音输入法组件（ESP-IDF）实施方案 · 修订版 v2

> **本次修订重点**：按你的最新澄清重写全部键位与布局规范；执行节奏改为 **先出「文档 + HTML 布局演示页」→ 你确认 → 再写实现代码**。

---

## 1. 摘要

在空目录 `d:\Codes\lvgl_pinyin_ime` 中从零构建一个 **可被 ESP-IDF 以组件形式引入** 的中文拼音输入法库
`lvgl_pinyin_ime`：

- 目标：**ESP32-S3-N16R8**（主）、**ESP32-S31**（正式支持）、**ESP32-P4**（顺带支持）
- 框架：**ESP-IDF 6.1.0**（`D:\Espressif\EIM\v6.1\esp-idf`）+ **LVGL 9.5**（组件仓库 `lvgl/lvgl`）
- 引擎：移植 **libgooglepinyin**（Google 拼音 Android 内核，Apache-2.0）→ 全拼 / 双拼 / 词组 / 整句
- UI：自绘 LVGL 组件，对齐参考图 —— 26 全键盘（含 4 个面板）+ 九键
- 能力：全拼 / 双拼（默认小鹤）/ 词组 / 联想（默认开）/ 分词（`'`）/ 词频自学习 / 模糊音（默认关）
- 数据：系统词库独立 flash 分区 + mmap；可选 SD 卡扩展词库（Python 生成的 bin）；字体由 `DreamHanSansSC-W17.ttf` 子集化为 GB2312 全集

---

## 2. 现状分析

### 2.1 工作区

```
d:\Codes\lvgl_pinyin_ime\
├── QQ20261001-184211.png          # 26 键参考图
├── 2026-10-01 19.06.47.png        # 九键参考图
└── DreamHanSansSC-W17.ttf         # 23 MB，静态 TTF（无 fvar），glyf 轮廓
```

全新项目，无既有代码与约定。参考图要素已读取确认：

- **26 键图**：候选栏一行 12 个汉字（`逆 你 尼 泥 擬 呃 匿 腻 溺 年 念 粘`）；键盘 4 行
- **九键图**：顶部输入框 → 字母组行 `w x y z` → 候选词行 `嘻嘻嘻嘻 我现在先 需要外援`（带 `^`）→ 5 列键区

### 2.2 环境（已实测）

| 项目 | 实测结果 |
| --- | --- |
| ESP-IDF | `D:\Espressif\EIM\v6.1\esp-idf`，版本 6.1.0 |
| 支持的 target | `components/soc` 下有 `esp32s3` / `esp32s31` / `esp32p4` ✅ |
| LVGL | IDF 自带 components **无 lvgl**；组件仓库 `lvgl/lvgl` 最新 **9.5.0** |
| 组件管理器缓存 | `D:\Espressif\EIM\.espressif\components` 为空（首次 build 联网拉取） |
| Python | `D:\Espressif\EIM\tools\idf-python\3.11.2\python.exe`（用于创建 venv） |
| IDF venv | 只有 5.4 / 5.5 的；**无 6.1 的**（首次构建由 EIM 自动创建） |
| `IDF_PATH` | 当前 shell 未设置，需 `esp-idf\export.ps1` |
| Node.js | **未检测到**（`lv_font_conv` 需要，见 3.2） |

### 2.3 已确认决策

| 议题 | 结论 |
| --- | --- |
| 目标芯片 | ESP32-S3-N16R8 + ESP32-S31（正式）、ESP32-P4（顺带） |
| 引擎路线 | 移植 libgooglepinyin |
| 内存/词库 | S3R8 带 8 MB PSRAM；词库可选从 SD 卡加载（bin，Python 生成），默认内置全量词库 |
| 字体 | GB2312 全量（6763 字），源文件 `DreamHanSansSC-W17.ttf` |
| 双拼默认 | 小鹤（自然码/微软/智能ABC/紫光/拼音加加内置可切） |
| 模糊音/联想 | 模糊音默认关；联想默认开 |
| **执行节奏** | **先写文档 + HTML 布局演示页，你确认布局后再开工实现** |

---

## 3. 需要你提供 / 下载的资源

### 3.1 代码仓库

| 用途 | 仓库 / 地址 |
| --- | --- |
| **主：引擎源码 + 词库** | `https://salsa.debian.org/input-method-team/libgooglepinyin.git`（Debian 维护的 libgooglepinyin 0.1.2，含 `dict_pinyin.dat`；原 code.google.com 已下线，这是最可靠副本） |
| 备选词库 | `http://archive.ubuntu.com/ubuntu/pool/universe/libg/libgooglepinyin/libgooglepinyin_0.1.2.orig.tar.bz2`（1.3 MB，含 `data/dict_pinyin.dat`） |
| 参考：Android PinyinIME | `https://github.com/lizhangqu/PinyinIME`（九键候选、联想、用户词典的成熟实现） |
| 参考：LVGL 官方 IME | `https://github.com/lvgl/lvgl`（`src/others/ime/lv_ime_pinyin.c`） |
| 参考：LVGL 中文 IME 插件 | `https://github.com/100askTeam/lv_chinese_ime` |
| 参考：双拼键位表 | `https://github.com/rime/rime-double-pinyin` |

建议放置：

```
d:\Codes\lvgl_pinyin_ime\reference\
├── libgooglepinyin\     # 主仓库
├── PinyinIME\           # 可选
└── lv_chinese_ime\      # 可选
```

### 3.2 必装软件

**Node.js LTS**（`lv_font_conv` 只有 Node 版）：

```powershell
winget install OpenJS.NodeJS.LTS
```

### 3.3 结论：哪些开源中文输入法做得最好

| 项目 | 是否适合 MCU | 取舍 |
| --- | --- | --- |
| **libgooglepinyin** | ✅ 无第三方依赖、词库 ~1 MB、占用低 | **采用为引擎** |
| **librime**（RIME） | ❌ 依赖 boost/yaml-cpp/leveldb | 只借鉴双数组 Trie 思路 |
| **libpinyin**（fcitx5） | ❌ Berkeley DB + 50 MB 模型 | 不采用 |
| **sunpinyin** | ❌ 43 MB 语言模型 | 不采用 |
| Android PinyinIME 原生 | ✅ 逻辑可借鉴 | 借鉴九键候选与联想交互 |
| LVGL 官方 `lv_ime_pinyin` | ⚠️ 仅"拼音→单字"，无词组/双拼/联想 | 仅借鉴候选面板 API 风格 |
| 100askTeam `lv_chinese_ime` | ⚠️ 词库简陋 | 借鉴按键矩阵组织方式 |

**结论：libgooglepinyin 是嵌入式中文拼音 IME 的最优选择。**

---

## 4. 【核心】键位与布局规范

> 本章是本次修订的重点，全部依据你的最新描述重写。HTML 布局演示页将严格按本章渲染。

### 4.1 26 键中文拼音布局

宽度单位 `u`（共 12u）：

```
┌──────────────────────────────────────────────────────────────┐
│  ┌────┐                                                      │  ← 拼音浮窗
│  │ ni │  （浮窗锚定在候选栏左上角，显示当前输入拼音）             │
│  └────┘                                                      │
├──────────────────────────────────────────────────────────────┤
│ [<]  逆  你  尼  泥  擬  呃  匿  腻  溺  年  念  粘  …  [>]    │  ← 候选栏（可横向滑动）
│  1u                    候选词区（flex 自适应）          1u      │     两端固定箭头翻页
├──────────────────────────────────────────────────────────────┤
│  符   q   w   e   r   t   y   u   i   o   p   ⌫               │  R1  12u
│  1    1   1   1   1   1   1   1   1   1   1   1                │
├──────────────────────────────────────────────────────────────┤
│ ⇧abc  a   s   d   f   g   h   j   k   l       ⏎              │  R2  12u
│  1    1   1   1   1   1   1   1   1   1       2                │
├──────────────────────────────────────────────────────────────┤
│ 分词   -   z   x   c   v   b   n   m   .   ,   :              │  R3  12u
│  1    1   1   1   1   1   1   1   1   1   1   1                │
├──────────────────────────────────────────────────────────────┤
│  ?123  ⌨        中/英      （CN）空格              ✓           │  R4  12u
│   2    2          2            4                   2          │
└──────────────────────────────────────────────────────────────┘
```


**按键行为表**

| 键 | 行为 |
| --- | --- |
| `符` (R1C1) | 打开 **数字/常用符号混合面板**（P1） |
| `⌫` | 有拼音缓冲 → 删拼音；缓冲空 → 删文本框字符；长按连删 |
| `⇧abc / ⇧ABC` | 单击：下一个字母大写；双击：大写锁定（键面切换为 `ABC`）；长按：切换中/英 |
| `⏎` | 换行（单行 textarea 下无效） |
| `分词` | 在全拼缓冲末尾插入 `'`（如 `pi` + 分词 + `ao` → `pi'ao` → 候选「皮袄」）；**英文模式下置灰**；**双拼模式下自动隐藏** |
| `-` `.` `,` `:` | 中文模式输出全角 `－` `。` `，` `：`；英文模式输出半角 `-` `.` `,` `:` |
| `?123` (R4C1) | 打开 **数字/常用符号混合面板**（P1），与 `符` 等效 |
| `⌨` | 在 **九键 ↔ 26 键** 之间切换 |
| `中/英` | 显示当前模式（"中文拼音模式" / "英文模式"），点击切换 |
| `（CN）空格` | 输出空格；键面中央显示当前中/英状态 |
| `✓` | 有拼音缓冲 → 上屏当前选中候选；无缓冲 → 收起键盘并发 `LV_EVENT_READY` |
| 候选栏 `[<]` `[>]` | 候选词上一页 / 下一页；到达边界时置灰 |

**拼音浮窗**：绝对定位在候选栏左上角外侧（`LV_ALIGN_OUT_TOP_LEFT` + 小偏移），
半透明底 + 圆角 chip 样式，显示当前拼音缓冲（含 `'`）；缓冲为空时隐藏。

### 4.2 面板体系（共 4 个面板）

```
P0 主键盘 ──[ 符 / ?123 ]──►  P1 数字 + 常用符号混合面板
                                 │
                                 ├─[ 左下角 ABC ]──► 返回 P0
                                 ├─[ 123 ]─────────► P2 纯数字面板 ─[ABC]─► P0
                                 └─[ 更多 ]────────► P3 全部符号列表面板 ─[ABC]─► P0
```

#### P1：数字 + 常用符号混合面板（10u 宽）

```
┌────────────────────────────────────────────────┐
│  1   2   3   4   5   6   7   8   9   0         │  R1  10×1u
├────────────────────────────────────────────────┤
│  @   #   ￥   %   &   *   -   +   (   )        │  R2  10×1u
├────────────────────────────────────────────────┤
│      更多           其他符号          ⌫        │  R3  4u + 4u + 2u
├────────────────────────────────────────────────┤
│  ABC        123         空格          ✓        │  R4  2u + 2u + 4u + 2u
└────────────────────────────────────────────────┘
```

| 键 | 行为 |
| --- | --- |
| `0-9` `@ # ￥ % & * - + ( )` | 直接输入对应字符 |
| `更多` | 打开 **P3 全部符号列表面板** |
| `其他符号` | 在当前面板内切换到另一批常用符号（如 `~ ! ? / \ | [ ] { } < >`） |
| `⌫` | 删除 |
| `ABC`（左下角） | 返回 P0 主键盘 |
| `123` | 打开 **P2 纯数字面板** |
| `空格` / `✓` | 空格 / 上屏 |

#### P2：纯数字面板

```
┌──────────────────────────────┐
│   1       2       3          │
│   4       5       6          │
│   7       8       9          │
│  ABC      0       ⌫          │
└──────────────────────────────┘
```

#### P3：全部符号列表面板

```
┌──────────────────────────────────────────────┐
│  [常用] [标点] [数学] [单位] [箭头]  ← 分类页签 │
├──────────────────────────────────────────────┤
│  ~ ! ? / \ | [ ] { } < >                     │
│  … — 、 。 ， ； ： ？！ 《》 〈〉 【】 〔〕      │
│  ± × ÷ ° ′ ″ Ω μ § ¥ € £ $                   │
│  ← → ↑ ↓ ⇒ ⇔ ∞ ≈ ≠ ≤ ≥ √                     │
├──────────────────────────────────────────────┤
│  ABC（返回主键盘）            ⌫               │
└──────────────────────────────────────────────┘
```

> P3 的分类页签与符号集合在 HTML 演示页给出具体方案供你增删。

### 4.3 九键布局

```
┌───────────────────────────────────────────────────────────┐
│  ┌────┐                                                   │  ← 拼音浮窗
│  │ 94 │                                                   │
│  └────┘                                                   │
├───────────────────────────────────────────────────────────┤
│ 候选拼音行：  xi   yi   zi   …              （横向可滑动）    │  ← 高亮当前项
│                ▲ 高亮（主题色底 + 反色字）                    │     点击可切换
├───────────────────────────────────────────────────────────┤
│ 候选词行： [<]  西   洗   系   细   析   希   息   稀  [>]   │  ← `<` `>` 翻页
├───────────────────────────────────────────────────────────┤
│   123     ,。?!    ABC     DEF      ⌫                     │  R1
├───────────────────────────────────────────────────────────┤
│   英文     GHI     JKL     MNO      分隔                   │  R2
├───────────────────────────────────────────────────────────┤
│   拼音     PQRS    TUV    WXYZ    ┌────────┐              │  R3
├───────────────────────────────────┤  确认  │              │     确认键 row_span = 2
│   🌐      选拼音       空格        └────────┘              │  R4
│                               col_span = 2                │
└───────────────────────────────────────────────────────────┘
```

| 键 | 行为 |
| --- | --- |
| `ABC` / `DEF` / `GHI` / `JKL` / `MNO` / `PQRS` / `TUV` / `WXYZ` | 追加数字 2–9 到九键缓冲；随后 `ime_t9_gen()` 刷新候选拼音行与候选词行 |
| `候选拼音行`点击 | 选中该拼音串为当前拼音 → 刷新候选词行；高亮该按钮 |
| `候选词行` `[<]` `[>]` | 候选词翻页（每页 `CAND_PAGE_SIZE` 个），边界置灰 |
| `123` | 打开 P1 数字/常用符号面板 |
| `,。?!` | 标点面板（切到 P1 的一个标点批次） |
| `⌫` | 删数字缓冲；缓冲空 → 删文本框字符 |
| `英文` | 切换到 26 键英文模式 |
| `拼音` | 切回中文拼音九键 |
| `分隔` | 插入 `'`（等同 26 键的"分词"） |
| `确认`（跨 2 行） | 上屏 |
| `🌐` | 切换 中文九键 / 英文 26 键（图标用内联 SVG 或文字，不用 emoji） |
| `选拼音` | 展开 / 收起候选拼音行（默认展开） |
| 底部 `空格`（跨 2 列） | 输出空格 |

### 4.4 英文模式

- 复用 26 键键盘，R1/R2/R3 行为同英文全键盘
- `分词` 置灰；`. , : -` 输出半角
- `中/英` / `（CN）空格` 显示 "英文模式"

---

## 5. 【本次立即交付】文档 + HTML 布局演示页

> **你已明确要求：先看到布局效果图，同意后再写实现代码。** 因此本次只交付文档与演示页。

### 5.1 交付物

| 文件 | 内容 |
| --- | --- |
| `docs/keymap.md` | 第 4 章规范的独立文档化版本（含 ASCII 布局图与按键行为表） |
| `docs/architecture.md` | 分层架构与数据流（第 6 章内容） |
| `docs/integration.md` | 组件引入、分区表、字体、SD 词库的接入说明（第 7/8 章内容） |
| `docs/dict_format.md` | 扩展词库 bin / bigram bin 格式规范 |
| **`mockup/ime_layout_demo.html`** | **单文件、零依赖的键盘布局演示页（本次重点）** |

### 5.2 HTML 演示页规格

- **单文件**，内联 CSS + JS，**不使用任何 CDN / 外部资源**，双击即可在浏览器打开
- 模拟手机屏幕外框，包含：
  - 假文本框（显示已上屏文字，如"请输入消息…"占位）
  - **拼音浮窗**（显示 `ni` / `94`，随模式变化）
  - 候选栏（`[<]` + 候选词 + `[>]`）
- **顶部调试栏**：一键切换预览所有面板
  `P0-26键中文` / `P0-26键英文` / `九键` / `P1-数字符号` / `P2-纯数字` / `P3-全部符号`
- **可交互**（但不含任何 IME 逻辑）：
  - 点按键有按下反馈（`transform` + 阴影）
  - 点候选栏 `[<]` `[>]` → 候选词换成下一批**静态示例数据**，边界置灰
  - 点九键候选拼音行 → 高亮切换，候选词行同步换成对应示例数据
  - 点 `⌨` → 26 键 ↔ 九键互相切换
  - 点 `符` / `?123` → P1；P1 点 `更多` → P3；P1 点 `123` → P2；P1/P2/P3 点 `ABC` → 回 P0
  - 点 `中/英` → 切中/英，按键面与标点随之变化
  - 点 `分词` / `分隔` → 拼音浮窗末尾追加 `'`（仅做显示演示）
- **布局数据全部集中在 JS 顶部的 `LAYOUT` 常量表里**，与将来的 C 键位表一一对应，便于比对与后续维护
- 严格不用 emoji；`🌐`、`⌨`、`⇧`、`⌫`、`⏎`、`✓` 用内联 SVG 或 Unicode 几何符号实现

### 5.3 你确认后的下一步

你在浏览器打开 `mockup/ime_layout_demo.html`，逐面板确认：
1. 行数、键位、宽度比例是否与预期一致
2. 候选栏 / 拼音浮窗的位置与尺寸
3. 九键候选拼音行的高亮样式与候选词行翻页箭头
4. P1/P2/P3 的按键内容与排版

确认或提出修改后，再进入第 9 章的 S1–S6 实现阶段。

---

## 6. 目标项目结构

```
d:\Codes\lvgl_pinyin_ime\
├── .gitignore
├── LICENSE                                  # Apache-2.0
├── README.md
├── CMakeLists.txt                           # 组件注册
├── Kconfig
├── idf_component.yml                        # lvgl/lvgl ^9.5
│
├── include/lvgl_pinyin_ime/
│   ├── lv_pinyin_ime.h                      # 公共 LVGL 组件 API
│   ├── lv_pinyin_ime_types.h                # 枚举 / 结构体 / 事件
│   └── ime_engine.h                         # 与 LVGL 无关的引擎 API
│
├── src/
│   ├── ui/
│   │   ├── lv_pinyin_ime.c                  # 根对象：生命周期、事件、状态机
│   │   ├── lv_pinyin_ime_cand.c             # 候选栏：拼音浮窗 + 候选词行 + < > 翻页
│   │   ├── lv_pinyin_ime_k26.c              # 26 键键盘 + P0
│   │   ├── lv_pinyin_ime_k9.c               # 九键键盘
│   │   ├── lv_pinyin_ime_panel.c            # P1/P2/P3 数字与符号面板
│   │   └── lv_pinyin_ime_style.c            # 默认样式（可被应用覆盖）
│   ├── core/
│   │   ├── ime_session.c                    # 会话状态机
│   │   ├── ime_engine_port.c                # 引擎门面（调 libgooglepinyin）
│   │   ├── ime_t9.c                         # 九键：数字串 → 候选拼音串
│   │   ├── ime_scheme.c                     # 六套双拼映射表
│   │   ├── ime_fuzzy.c                      # 模糊音变体（默认关）
│   │   ├── ime_assoc.c                      # 联想（默认开）
│   │   ├── ime_utf.c                        # UTF-8 <-> UTF-16LE
│   │   ├── ime_hash.c                       # 开放寻址哈希表
│   │   ├── ime_trie.c                       # 字典树
│   │   └── ime_alloc.c                      # PSRAM 优先分配器
│   ├── data/
│   │   ├── ime_dict.c                       # 词库装载（分区/SD/内置三选一）
│   │   ├── ime_dict_vfs.c                   # 只读 VFS：dict 分区 mmap → /dict/dict_pinyin.dat
│   │   ├── ime_extdict.c                    # SD 扩展词库 bin 解析
│   │   ├── ime_userdb.c                     # 用户词典 FAT 挂载 + 落盘节流
│   │   └── ime_font.c                       # 字体分区 mmap + lv_binfont
│   └── third_party/libgooglepinyin/         # 上游源码 + ESP-IDF 适配补丁
│       ├── CMakeLists.txt
│       ├── include/  src/
│       └── UPSTREAM.md                      # 来源 commit + 补丁清单
│
├── data/
│   ├── dict/dict_pinyin.dat                 # 系统词库（fetch_dict.py 放入）
│   ├── ext/ext_wordlist_sample.txt
│   └── charset/gb2312.txt
│
├── partitions/
│   ├── ime_16mb.csv
│   └── ime_8mb.csv
│
├── generated/                               # 构建产物（.gitignore）
│
├── tools/                                   # PC 侧 Python 工具链（venv 隔离）
│   ├── requirements.txt                     # fonttools, pypinyin, requests
│   ├── setup_env.ps1                        # 创建 venv 并装依赖（禁止装全局）
│   ├── fetch_dict.py
│   ├── gen_charset.py
│   ├── gen_font.py
│   ├── gen_extdict.py
│   ├── gen_bigram.py
│   └── flash_dict.ps1
│
├── mockup/
│   └── ime_layout_demo.html                 # ★ 本次交付的布局演示页
│
├── examples/k26_k9_demo/                     # 可直接 build/flash 的示例
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── sdkconfig.defaults.esp32s31
│   ├── sdkconfig.defaults.esp32p4
│   └── main/{CMakeLists.txt, main.c}
│
├── test_apps/host_core_test/                # 纯 C 模块主机端单测
│
└── docs/
    ├── keymap.md                            # ★ 第 4 章
    ├── architecture.md
    ├── integration.md
    └── dict_format.md
```

---

## 7. 架构与实现要点（布局确认后执行）

### 7.1 分层

```
UI 层（唯一依赖 lvgl）
  根对象 → 候选栏（拼音浮窗 + 候选词行 + < >）→ 键盘（K26 / K9 / P1 / P2 / P3）

会话层 ime_session.c
  拼音缓冲 / 分词 / 翻页游标 / 上屏 / 联想触发 / 模式切换

引擎层（不依赖 LVGL，可主机端单测）
  ime_engine_port → libgooglepinyin（全拼 / 双拼 / 词组 / 整句）
  ime_t9          → 数字串 → 候选拼音串（Trie 切分）
  ime_scheme      → 双拼键位映射
  ime_fuzzy       → 模糊音变体
  ime_assoc       → 联想（哈希表 bigram + 首字联想 + 用户近期词）

数据层
  ime_dict / ime_extdict / ime_userdb / ime_font
```

### 7.2 libgooglepinyin 移植补丁清单

| # | 位置 | 内容 |
| --- | --- | --- |
| P1 | `mystdlib.cpp` | `malloc/free/new` → `ime_alloc.c` 的 `ime_malloc/ime_free`（`MALLOC_CAP_SPIRAM` 优先） |
| P2 | `pinyin_ime.cpp` / `userdict.cpp` | POSIX `mmap/open/flock` → `fopen/fread` 或 `#ifdef ESP_PLATFORM` 分支 |
| P3 | `dict_trie.cpp` | 可选零拷贝路径：源来自 mmap 的 dict 分区时直接引用映射地址，省 1.1 MB PSRAM |
| P4 | `sync.cpp` | 确认 pthread 静态初始化在 ESP-IDF 下可用 |
| P5 | `userdict.cpp` | `save_dict` 改为"脏标记 + 显式 flush" |
| P6 | 全局 | `printf` 调试输出 → `ESP_LOGx` |
| P7 | 编译 | `third_party` 目录整体 `-w`，保持整体构建零警告 |

**上游 C API（已核对）**

```c
bool   im_open_decoder(const char *fn_sys_dict, const char *fn_usr_dict);
bool   im_open_decoder_fd(int sys_fd, long start_offset, long length, const char *fn_usr_dict);
void   im_close_decoder(void);
void   im_set_max_lens(size_t max_sps_len, size_t max_hzs_len);
void   im_flush_cache(void);
size_t im_search(const char *sps_buf, size_t sps_len);   /* ' 作为音节分隔 */
size_t im_delsearch(size_t pos, bool is_backspace, bool is_clear);
void   im_reset_search(void);
size_t im_get_sps_str(char **sps_str);
char16 *im_get_candidate(size_t cand_id, char16 *cand_str, size_t max_len);
```

> ⚠️ **执行时第一件事**：上游未公开"设置双拼方案"的 C API。若确认缺失，新增
> `third_party/libgooglepinyin/src/ime_capi_extra.cpp` 包装内部 `SetDpScheme`（仅新增，不改上游逻辑）。

### 7.3 需求 6「哈希表 + 字典树」的落点

| 结构 | 用途 |
| --- | --- |
| 哈希表 `ime_hash.c` | 联想 bigram 查询、用户词频查询、音节表快速校验 |
| 字典树 `ime_trie.c` | 九键数字 → 拼音切分（如 `94` → `xi`/`yi`/`zi`；`9464` → `xing`/`xini`/`zheli`）、扩展词库前缀检索、联想首字检索 |

### 7.4 数据与分区

**分区表 `partitions/ime_16mb.csv`（S3-N16R8，16 MB）**

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
dict,     data, 0x40,    0x10000,  0x180000,     # 1.5 MB 系统词库
font,     data, 0x41,    ,         0x300000,     # 3 MB LVGL 二进制字体
usr,      data, fat,     ,         0x100000,     # 1 MB 用户词频（FAT + 磨损均衡）
factory,  app,  factory, ,         0x800000,     # 8 MB 应用
```

合计 ≈ 13.6 MB < 16 MB ✅。另给 `ime_8mb.csv`(4 MB app、无 font 分区、字体内嵌)。

- `dict` 分区通过 `esp_partition_mmap()` + 自写只读 VFS（`esp_vfs_register`）暴露为
  `/dict/dict_pinyin.dat`，上游 `fopen` 即可读
- `font` 分区存 `lv_font_ime_20.bin` + `lv_font_ime_16.bin` 的合并镜像，
  `lv_binfont_create_from_buffer()` 直接加载
- 烧录用 ESP-IDF 原生 `esptool_py_flash_to_partition()` 或 `parttool.py`

**扩展词库 bin（`ime_ext.bin`，SD 卡可选，Python 生成）**

```
magic "IMED"(4) | version u16 | reserved u16 | record_count u32 | total_size u32
逐条： u8 pinyin_len, pinyin[], u8 word_len, word_utf8[], u32 freq
```

### 7.5 工具链

| 脚本 | 产出 |
| --- | --- |
| `setup_env.ps1` | 用 `D:\Espressif\EIM\tools\idf-python\3.11.2\python.exe -m venv venv` 创建 `tools/venv`，再装 `requirements.txt`（**禁止装到全局**） |
| `fetch_dict.py` | `data/dict/dict_pinyin.dat`，校验大小与魔数 |
| `gen_charset.py` | 由 Python `codecs` 枚举 GB2312，并 `--extra-scan dict_pinyin.dat` 扫描二进制中出现的 BMP 汉字，输出"未覆盖字符"报告 |
| `gen_font.py` | 调 `npx lv_font_conv --font DreamHanSansSC-W17.ttf --bpp 4 --size 20/16 --format bin --no-compress --symbols <charset>` → `lv_font_ime_20.bin` / `lv_font_ime_16.bin` / `font_partition.bin` / `font_layout.json` |
| `gen_extdict.py` | 文本词表 → `ime_ext.bin` |
| `gen_bigram.py` | 语料 → `ime_bigram.bin`（未提供语料则不生成，联想降级） |
| `flash_dict.ps1` | 封装 `parttool.py write_partition` |

---

## 8. Kconfig 选项

```
LV_PINYIN_IME_DEFAULT_MODE            # K26 / K9，默认 K26
LV_PINYIN_IME_DOUBLE_SCHEME           # none/小鹤/自然码/微软/ABC/紫光/拼音加加，默认 小鹤
LV_PINYIN_IME_FUZZY_DEFAULT           # bool，默认 n
LV_PINYIN_IME_ASSOC_DEFAULT           # bool，默认 y
LV_PINYIN_IME_MAX_CANDIDATES          # 默认 60
LV_PINYIN_IME_CAND_PAGE_SIZE          # 默认 8
LV_PINYIN_IME_K9_MAX_PY_CAND          # 默认 20
LV_PINYIN_IME_MAX_PINYIN_LEN          # 默认 32
LV_PINYIN_IME_DICT_SRC                # PARTITION(默认)/SDCARD/EMBEDDED
LV_PINYIN_IME_DICT_PARTITION          # 默认 "dict"
LV_PINYIN_IME_DICT_SD_PATH            # 默认 "/sdcard/dict_pinyin.dat"
LV_PINYIN_IME_USER_PARTITION          # 默认 "usr"
LV_PINYIN_IME_USER_AUTOSAVE_EVERY     # 默认 10
LV_PINYIN_IME_FONT_SRC                # PARTITION(默认)/EMBEDDED
LV_PINYIN_IME_FONT_PARTITION          # 默认 "font"
LV_PINYIN_IME_FONT_SIZE_BIG           # 默认 20
LV_PINYIN_IME_FONT_SIZE_SMALL         # 默认 16
LV_PINYIN_IME_EXT_DICT_ENABLE         # 默认 n
LV_PINYIN_IME_EXT_DICT_PATH           # 默认 "/sdcard/ime_ext.bin"
```

---

## 9. 实施阶段（布局确认后执行）

| 阶段 | 内容 | 交付物 |
| --- | --- | --- |
| **S-1** | **文档 4 篇 + `mockup/ime_layout_demo.html`** | ★ 你审核布局 |
| **S0** | 项目骨架：CMakeLists / Kconfig / idf_component.yml / 分区表 / example 骨架 / .gitignore / LICENSE / README | 三目标 `idf.py build` 通过 |
| **S1** | 移植 libgooglepinyin：vendor + 补丁 P0–P8 + `ime_engine_port.cpp` / `ime_alloc.c` / `ime_dict*.c` | 目标端能打印 "ni → 你" 候选 |
| **S2** | Python 工具链：setup_env / fetch_dict / gen_charset / gen_font / gen_extdict | 词库 + 字体分区镜像可用 |
| **S3** | UI 主体：根对象 + 候选栏（含拼音浮窗）+ K26 + P1/P2/P3 + style，全拼输入 / 分词 / 翻页 / 上屏 | 26 键全流程可用 |
| **S4** | 九键：`lv_pinyin_ime_kb.c`(K9) + `ime_t9.c` + `ime_trie.c` + `ime_hash.c` | 九键候选拼音行 + 候选词行 + 翻页可用 |
| **S5** | 双拼 + 模糊音 + 联想 + 用户词频学习 + `ime_extdict.c` | 全功能达成 |
| **S6** | 完成文档、example、`test_apps/host_core_test`、三目标回归 | 可交付库 |

### 9.1 实施现状（截至本次）

| 阶段 | 状态 | 验证方式 |
| --- | --- | --- |
| S-1 | 已完成 | `docs/` 四篇；`mockup/` 未做（改为直接用固件看效果） |
| S0 | 已完成 | 三目标 `idf.py build` |
| S1 | **已完成** | 引擎在主机端跑通：`ni`→你、`piao`→票、`pi'ao`→皮奥、`women`→我们、`我们`→联想候选；`test_apps/host_core_test` 70 项断言全绿 |
| S2 | **已完成** | `setup_env.ps1` / `gen_dict.py`（1 068 442 B，格式固定为 32 位，可复现）/ `gen_charset.py`（7 549 字 + 覆盖率报告）/ `gen_font.py`（默认 `--format c`：20 px 与 16 px 各 2 bpp，位图约 675 KB + 437 KB，并把 lv_font_conv 的 LVGL 8 输出修正成 LVGL 9 结构；`--format bin` 生成分区镜像）/ 辅助的 `check_charset.py`（按 LVGL 规则遍历生成字体的 cmap）、`check_dict.py`、`measure_fonts.py`、`inspect_font.py`、`flash_dict.ps1`；`gen_extdict.py` 随 S5 做 |
| S3 | **已完成** | 26 键全键位（P0）+ P1/P2/P3 + 候选栏 + 拼音浮窗 + 翻页 + 分词；字体默认**编进固件**（`FONT_SRC_EMBEDDED`），可切回字体分区；按键为方角小圆角；编译通过（三目标） |
| S4 | **已完成** | 九键键盘 + 候选拼音行（高亮/点击切换）+ `ime_t9`（`94`→xi/yi/zi，`9464`→xing）+ `ime_trie`/`ime_hash` + 单测 |
| S5 | 未开始 | — |
| S6 | 进行中 | `host_core_test` 70 项断言、`host_ui_render`（真实 LVGL 渲染 + 键位文本 dump，`-DIME_HOST_REAL_FONT=ON` 时用真实字体）、`host_font_probe`（校验生成字体能否被本机 LVGL 取到字形与位图）可用；`test_apps/idf_component_build` 三目标（s3/s31/p4）编译通过，均启用内嵌字体 |

**与原计划的两处偏差（已按更简单的方案落地）**

1. **词库不再下载**：原计划下载 `dict_pinyin.dat`；实际改为用仓库内上游原始词表 +
   仓库内 dictbuilder 现场生成（`tools/gen_dict.py`），可复现且无需联网。
2. **P1–P8 补丁不再改上游代码**：除 `include/dictdef.h` 的一处条件宏（P0）外，
   全部通过构建系统（链接器 `--wrap`、`-include` 兼容头、`-w`）实现，
   便于与上游做 diff。详见 `src/third_party/libgooglepinyin/UPSTREAM.md`。
3. **字体不再用 `lv_binfont_create_from_buffer()`**：该 API 需要打开
   `LV_USE_FS_MEMFS`；改为复用只读 VFS 把字体分区暴露成 `/ime/font.bin`，
   直接 `lv_binfont_create(path)`，少一个 LVGL 配置依赖（原假设 A2 已按此降级）。

**已知能力缺口（S5 的工作）**

- 上游 libgooglepinyin **没有双拼支持**，需要在 `ime_scheme.c` 自建方案表。
- 上游原始词表词条偏少，`pi'ao` 目前只能给出「皮奥」这类二字组合；
  「皮袄」需要 S5 的扩展词库（`ime_ext.bin`）或用户词频学习。
- 字表默认只含 GB2312：引擎能输出但 GB2312 之外的 9 716 个汉字会显示为缺字方框，
  需要更大的字体分区才能全量覆盖（`tools/measure_fonts.py` 可测）。

---

## 10. 验证步骤

### 10.1 S-1（本次）
- 浏览器打开 `mockup/ime_layout_demo.html`，逐面板核对第 4 章规范
- `docs/` 四篇文档内容自检（路径、命令、格式规范与实际环境一致）

### 10.2 S0 骨架
```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd d:\Codes\lvgl_pinyin_ime\examples\k26_k9_demo
idf.py set-target esp32s3  ; idf.py build
idf.py set-target esp32s31 ; idf.py build
idf.py set-target esp32p4  ; idf.py build
```
标准：三个 target 均 `Project build complete`，无本组件产生的警告。

### 10.3 S1 引擎
`test_apps/host_core_test`（纯 C，主机端）断言：`ime_utf` 往返、`ime_hash`/`ime_trie` 正确性、
六套双拼表与小鹤官方键位一致、`ime_t9_gen("94")` 含 `xi/yi/zi`、`ime_extdict` 能解析生成的 bin。

### 10.4 S2 工具链
```powershell
cd d:\Codes\lvgl_pinyin_ime\tools
powershell -ExecutionPolicy Bypass -File setup_env.ps1
.\venv\Scripts\python.exe fetch_dict.py
.\venv\Scripts\python.exe gen_charset.py --extra-scan ..\data\dict\dict_pinyin.dat
.\venv\Scripts\python.exe gen_font.py
```
标准：`font_coverage_report.txt` 中"未覆盖字符"为空或均为已知可接受项。

### 10.5 S3–S5（需硬件，你烧录确认）
1. 布局与 HTML 演示页一致
2. 输入 `ni` → 候选栏出现 `你/尼/泥/…`；`[<]` `[>]` 可翻页且边界置灰
3. **核心验收**：输入 `pi` → 按 `分词` → 输入 `ao` → 候选出现 **皮袄**（而非 `piao/票`）
4. 拼音浮窗正确显示当前拼音；缓冲清空后隐藏
5. `⇧` 单击 / 双击 / 长按 分别 = 单字母大写 / 大写锁定 / 中英切换
6. `符` / `?123` → P1；P1 `更多` → P3；P1 `123` → P2；`ABC` 均能返回
7. 九键：数字键 → 候选拼音行出现并高亮；候选词行同步；两行均可翻页/切换
8. 双拼（小鹤）：如 `aa` → 长安/常按… 类候选；切换方案后行为随之变化
9. 联想：上屏「我们」后出现「的/是/要…」
10. 词频学习：多次选同一候选后前移；重启仍生效
11. SD 扩展词库：放入 `ime_ext.bin` 后自定义词进入候选；拔出不影响基本功能

### 10.6 S6
`esp32s31` / `esp32p4` 至少编译验证；有硬件则重复 10.5 的 1/2/3/7 项。

---

## 11. 风险与限制

| 风险 | 缓解 |
| --- | --- |
| 上游 C API 不暴露双拼方案设置 | 新增 `ime_capi_extra.cpp` 包装内部 `SetDpScheme` |
| 上游含 POSIX 专属调用 | 逐文件 `#ifdef ESP_PLATFORM` 适配（P2） |
| 词库存在 GB2312 之外的汉字 | `gen_charset.py --extra-scan` 扫描词库并输出未覆盖清单，可一键扩展字表重生成字体 |
| 字体子集后约 1.4 MB（4bpp/20px） | 可切 2bpp(~700 KB) / 1bpp(~350 KB)；分区已预留 3 MB |
| ESP32-S31 为新品，显示驱动适配可能滞后 | 该目标先保证 `idf.py build` 通过；显示由 `esp_lvgl_port` 负责 |
| 首次构建需联网拉取 LVGL | 记录 `managed_components/` 复用方式；必要时本地 vendor |
| 首次构建触发 EIM 创建 idf6.1 的 Python 环境 | 提前跑一次 `export.ps1` 预热 |

**关键假设（执行时验证，不符则按注明的降级路径处理）**

- A1：`im_search()` 接受 `'` 作为分隔符。若拒绝 → 在 `ime_engine_port.c` 按 `'` 拆分为多次前缀递进调用
- A2：LVGL 9.5 提供 `lv_binfont_create_from_buffer()`。若缺失 → 退化为字体内嵌 C 数组
- A3：`lv_font_conv --format bin --no-compress` 产物可被直接解析。若需压缩 → 改用 `lv_binfont_create(path)`
- A4：ESP32-S31 / P4 的 `esp_partition_mmap` 与 FAT+VFS 行为与 S3 一致
- A5：P4 无 Wi-Fi，但本项目不需要网络，故"顺带支持"仅多一份 `sdkconfig.defaults` 与构建验证

---

## 12. 明确不做

- 云端词库 / 云拼音
- 手写、语音输入；只做数字与符号面板
- OTA 与词库在线更新
- ESP32-S2 / C3 / C6 等其它芯片适配
- 重写 libgooglepinyin 算法（只移植 + 适配 + 叠加扩展层）
- Emoji 面板（本次 P3 只做符号）