# 架构与数据流

## 1. 分层

```
UI 层（唯一依赖 lvgl）
  根对象 → 候选栏（拼音浮窗 + 候选词行 + < >）→ 键盘（K26 / K9 / P1 / P2 / P3）

会话层 ime_session.c
  拼音缓冲 / 分词 / 翻页游标 / 上屏 / 模式切换 / 9 键缓冲

引擎层（不依赖 LVGL，可主机端单测）
  ime_engine_port.cpp → libgooglepinyin（全拼 / 词组 / 整句）
  ime_t9              → 数字串 → 候选拼音串
  ime_trie            → 数字串 → 候选音节（查表）
  ime_hash            → 开放寻址哈希表（通用结构）

数据层
  ime_dict / ime_dict_vfs / ime_font
```

约束：

- `src/core/`、`src/data/`、`src/third_party/` **不得** include `lvgl.h`，保证可在主机端
  `test_apps/host_core_test` 独立编译与断言。
- `src/ui/` 只通过 `ime_session.c` 暴露的接口驱动引擎，不直接调用 libgooglepinyin。

> 计划中的双拼（`ime_scheme`）、模糊音（`ime_fuzzy`）、联想（`ime_assoc`）、用户词典
> （`ime_userdb`）、SD 扩展词库（`ime_extdict`）**尚无对应文件**，属"规划中"，见
> [README 的功能与现状](../README_zh-cn.md#功能与现状)。

## 2. 模块清单

| 文件 | 职责 |
| --- | --- |
| `src/ui/lv_pinyin_ime.c` | 根对象：生命周期、尺寸/字体全局状态、事件分发、退格长按、按键分发 |
| `src/ui/lv_pinyin_ime_cand.c` | 候选栏：拼音浮窗 + 候选词行 + `<` `>` 翻页 |
| `src/ui/lv_pinyin_ime_kb.c` | 键盘表与构建：26 键 / 九键 / P1–P3 面板、键位布局、重建 |
| `src/ui/lv_pinyin_ime_style.c` | 默认样式，可被应用覆盖 |
| `src/ui/mic_icon.c` | 语音键的麦克风图标（内嵌位图） |
| `src/ui/lv_pinyin_ime_internal.h` | UI 各编译单元共享的私有状态，不对外 |
| `src/core/ime_session.c` | 会话状态机：拼音缓冲、分词、翻页、上屏、模式、9 键 |
| `src/core/ime_engine_port.cpp` | 引擎门面，包装 libgooglepinyin C API |
| `src/core/ime_t9.c` | 数字串 → 候选拼音串 |
| `src/core/ime_trie.c` | 数字串 → 候选音节（前缀索引） |
| `src/core/ime_hash.c` | 开放寻址哈希表 |
| `src/core/ime_alloc.c` | PSRAM 优先分配器 |
| `src/data/ime_dict.c` | 词库装载（分区 / SD / 内嵌三选一） |
| `src/data/ime_dict_vfs.c` | 只读 VFS：dict 分区 mmap → `/dict/dict_pinyin.dat` |
| `src/data/ime_font.c` | 字体加载（内嵌 C 数组 / `font` 分区 `lv_binfont`） |
| `src/voice/ime_voice.c` | 语音会话状态机与 40 ms 音频节拍 |
| `src/voice/ime_voice_proto.c` | 请求帧构造与返回帧解析 |
| `src/voice/ime_voice_crypto.c` | base64 / SHA-256 / HMAC-SHA256 |
| `src/voice/ime_voice_ui.c` | 语音与键盘/候选栏的粘合层 |

## 3. 核心数据流

### 3.1 全拼输入

```
按键 q … i
  → lv_pinyin_ime_kb.c / lv_pinyin_ime.c  → ime_session_push_letter('i')
  → ime_session 维护拼音缓冲 "ni"（含分词符 '）
  → ime_engine_port.ime_engine_search("ni")
  → libgooglepinyin im_search() → 候选 UTF-16 列表
  → 转 UTF-8 → 回填候选栏（lv_pinyin_ime_cand.c）
```

### 3.2 上屏

```
点击候选 / 按 ✓
  → ime_session_commit_candidate(index)
  → 写入绑定的 lv_textarea
```

> 用户词频学习与联想已预留接口（`ime_engine_predict()` 封装了上游
> `im_get_predicts()`），但 UI 尚未调用，也还没有用户词典落盘模块。

### 3.3 九键输入

```
按键 PQRS(7) → 数字缓冲 "7"
  → ime_t9_gen("7") → 候选拼音串（查音节表）
  → 点击候选拼音 "xi" → 当作全拼缓冲，进入 3.1 流程
```

## 4. libgooglepinyin 移植补丁清单

**实际落地方案见 `src/third_party/libgooglepinyin/UPSTREAM.md`**。上游源码除
`include/dictdef.h` 中的一处条件宏（P0）外未做任何修改，其余全部通过构建系统与
`port/pinyinime_compat.h` 实现，便于与上游做 diff。

| # | 计划里的做法 | 实际做法 |
| --- | --- | --- |
| P0 | （新增）关掉模型构建器 | `dictdef.h` 增加 `PINYINIME_RUNTIME_ONLY` 守卫；固件编译时定义该宏，主机侧不定义 |
| P1 | 改 `mystdlib.cpp` 里的 malloc | 链接器 `--wrap=malloc/free/calloc/realloc` → `src/core/ime_alloc.c`（PSRAM 优先）；无需改上游 |
| P2 | 改 `mmap/open/flock` | 上游运行时本就用 `fopen/fread`；写回路径 `userdict.cpp` 用 `open/lseek/write/ftruncate`，ESP-IDF 全部支持 |
| P3 | 零拷贝 mmap | 不需要：`src/data/ime_dict_vfs.c` 已把 dict 分区映射后以只读文件暴露给 `fopen` |
| P4 | 确认 pthread 静态初始化 | ESP-IDF 自带 pthread；仅主机（MinGW）走 `std::recursive_mutex` 替身 |
| P5 | 用户词典脏标记 | 上游 `userdict.cpp` 本来就只在 `close_dict()` 写回；节流策略属 S5，尚未落地 |
| P6 | `printf` → `ESP_LOGx` | 上游运行时 `printf` 全部被 `kPrintDebugX = false` 关掉，无需改动 |
| P7 | `third_party` 整体 `-w` | 已实现，且只作用于该组件 |

### 4.1 上游 C API（已核对）

```c
bool   im_open_decoder(const char *fn_sys_dict, const char *fn_usr_dict);
bool   im_open_decoder_fd(int sys_fd, long start_offset, long length, const char *fn_usr_dict);
void   im_close_decoder(void);
void   im_set_max_lens(size_t max_sps_len, size_t max_hzs_len);
void   im_flush_cache(void);
size_t im_search(const char *sps_buf, size_t sps_len);   /* ' 作为音节分隔 */
size_t im_delsearch(size_t pos, bool is_pos_in_splid, bool clear_fixed_this_step);
void   im_reset_search(void);
const char *im_get_sps_str(size_t *decoded_len);
char16 *im_get_candidate(size_t cand_id, char16 *cand_str, size_t max_len);
size_t im_get_spl_start_pos(const uint16 *&spl_start);
size_t im_get_predicts(const char16 *his_buf, char16 (*&pre_buf)[kMaxPredictSize + 1]);
```

关键实测结论：

- `im_search()` 的缓冲区是 **ASCII 字节**（不是 UTF-16），长度按字符计；`pi'ao` 可被
  正确切分为两个音节（主机端实测：`pi'ao` → 皮奥，`piao` → 票）。
- `MatrixSearch::init()` 不接受 NULL 用户词库路径（但接受不存在的路径），因此
  `ime_engine_open()` 在"仅内存用户词库"时传入空串。
- `im_get_predicts()` 使用 C++ 引用参数，C 侧由 `ime_engine_predict()` 包装。
- **上游完全没有双拼支持**（无 `SetDpScheme`、无方案表），双拼需自行实现方案映射后再
  喂全拼给引擎，属规划中。

## 5. 解码算法：unigram 引擎（联想层尚未落地）

### 5.1 引擎层（上游 libgooglepinyin，unigram）

```
ime_engine_search(全拼串)
  → im_search() → MatrixSearch
  → spellingtrie.cpp   音节 trie 切分
  → lpicache.cpp       词条缓存查询
  → NGram::get_uni_psb()  逐个词条打分
  → extend_mtrx_nd()   多音节句子的动态规划分词
```

`NGram` 只提供 unigram：`ngram.h` 里仅 `get_uni_psb()` 与 `build_unigram()`，
**没有**任何 bigram 构建或查询函数。因此单个搜索步内，词 *n* 的频率不影响词 *n+1*
的得分；多音节句子的切分靠 `extend_mtrx_nd()` 在这些 unigram 得分上做动态规划，
而不是靠词间转移概率。这是 2009 年上游遗留的真实短板。

### 5.2 联想层（规划中）

计划在上屏之后由联想层预测下一个词，2-gram 信息放在这一层（bigram 哈希表
`ime_hash.c` 已实现，但暂无调用方；数据计划来自离线生成的 `ime_bigram.bin`）。
**该层尚未落地**，因此当前实际生效的只有 5.1 的 unigram 引擎。

### 5.3 结论

- **引擎**是纯 unigram，这是继承自上游的真实限制；
- **项目**计划在其上另加一层 bigram 联想，但还没有代码；
- 与 libpinyin / sunpinyin 比较时，准确说法是「libgooglepinyin 引擎仅用 unigram
  打分」，而非「本项目仅用 unigram」。

九键路径自身不带任何打分：`ime_trie.c` 数字串→候选音节、`ime_t9.c` 展开，都是查表，
随后把拼好的全拼串交给同一个引擎。排序逻辑只有一处 —— 引擎。

## 6. 哈希表与字典树的落点

| 结构 | 用途 | 实现说明 |
| --- | --- | --- |
| 哈希表 `ime_hash.c` | 计划用于联想 bigram 与用户词频（尚未接线） | 开放寻址 + 线性探测 + 墓碑槽位，容量固定为 2 的幂，键为组件自持副本 |
| 前缀索引 `ime_trie.c` | 九键数字 → 拼音（`94` → `xi`/`yi`/`zi`；`9464` → `xing`/`ying`），最短拼写优先 | 音节表按数字串排序，全表扫描，不建节点表、不额外占 RAM |

## 7. 内存预算（ESP32-S3-N16R8，8 MB PSRAM）

| 项 | 规模 | 位置 |
| --- | --- | --- |
| dict 分区映射 | 分区约 1.5 MB（实际词库约 1.1 MB） | flash → mmap |
| 候选 / 会话缓冲 | < 64 KB | 内部 RAM |
| 用户词典（FAT） | 1 MB 分区（规划中，尚未使用） | flash |
| 字体分区映射 | ≤ 3 MB（`PARTITION` 模式下） | flash → mmap |
| 引擎运行时（Trie 等） | 约 1.5–2 MB（零拷贝路径下更低） | PSRAM 优先 |

## 8. 相关文档

- 键位与布局：[keymap.md](./keymap.md)
- 组件接入：[integration.md](./integration.md)
- 词库二进制格式：[dict_format.md](./dict_format.md)
- 功能现状与算法概览：[../README_zh-cn.md](../README_zh-cn.md)
