# 架构与数据流

## 1. 分层

```
UI 层（唯一依赖 lvgl）
  根对象 → 候选栏（拼音浮窗 + 候选词行 + < >）→ 键盘（K26 / K9 / P1 / P2 / P3）

会话层 ime_session.c
  拼音缓冲 / 分词 / 翻页游标 / 上屏 / 联想触发 / 模式切换

引擎层（不依赖 LVGL，可主机端单测）
  ime_engine_port → libgooglepinyin（全拼 / 双拼 / 词组 / 整句）
  ime_t9          → 数字串 → 候选拼音串（Trie 切分）
  ime_scheme      → 双拼键位映射（六套）
  ime_fuzzy       → 模糊音变体（默认关）
  ime_assoc       → 联想（哈希表 bigram + 首字联想 + 用户近期词）

数据层
  ime_dict / ime_extdict / ime_userdb / ime_font
```

约束：

- `src/core/`、`src/data/`、`src/third_party/` **不得** include `lvgl.h`，保证可在主机端
  `test_apps/host_core_test` 独立编译与断言。
- `src/ui/` 只通过 `ime_session.c` 暴露的接口驱动引擎，不直接调用 libgooglepinyin。

## 2. 模块清单

| 文件 | 职责 |
| --- | --- |
| `src/ui/lv_pinyin_ime.c` | 根对象：生命周期、事件分发、状态机入口 |
| `src/ui/lv_pinyin_ime_cand.c` | 候选栏：拼音浮窗 + 候选词行 + `<` `>` 翻页 |
| `src/ui/lv_pinyin_ime_k26.c` | 26 键键盘（P0）+ 英文模式 |
| `src/ui/lv_pinyin_ime_k9.c` | 九键键盘 + 候选拼音行 |
| `src/ui/lv_pinyin_ime_panel.c` | P1 / P2 / P3 数字与符号面板 |
| `src/ui/lv_pinyin_ime_style.c` | 默认样式，可被应用覆盖 |
| `src/core/ime_session.c` | 会话状态机：拼音缓冲、分词、翻页、上屏、联想 |
| `src/core/ime_engine_port.c` | 引擎门面，包装 libgooglepinyin C API |
| `src/core/ime_t9.c` | 数字串 → 候选拼音串 |
| `src/core/ime_scheme.c` | 六套双拼映射表 |
| `src/core/ime_fuzzy.c` | 模糊音变体（默认关） |
| `src/core/ime_assoc.c` | 联想（默认开） |
| `src/core/ime_utf.c` | UTF-8 ↔ UTF-16LE |
| `src/core/ime_hash.c` | 开放寻址哈希表 |
| `src/core/ime_trie.c` | 字典树 |
| `src/core/ime_alloc.c` | PSRAM 优先分配器 |
| `src/data/ime_dict.c` | 词库装载（分区 / SD / 内置三选一） |
| `src/data/ime_dict_vfs.c` | 只读 VFS：dict 分区 mmap → `/dict/dict_pinyin.dat` |
| `src/data/ime_extdict.c` | SD 扩展词库 bin 解析 |
| `src/data/ime_userdb.c` | 用户词典 FAT 挂载 + 落盘节流 |
| `src/data/ime_font.c` | 字体分区 mmap + `lv_binfont` |

## 3. 核心数据流

### 3.1 全拼输入

```
按键 q … i
  → lv_pinyin_ime_k26.c  → ime_session_push_letter('i')
  → ime_session 维护拼音缓冲 "ni"（含分词符 '）
  → ime_engine_port.ime_engine_search("ni")
  → libgooglepinyin im_search() → 候选 UTF-16 列表
  → ime_utf 转 UTF-8 → 回填候选栏（lv_pinyin_ime_cand.c）
```

### 3.2 上屏与联想

```
点击候选 / 按 ✓
  → ime_session_commit(index)
  → 写入绑定的 lv_textarea
  → ime_assoc 查询 bigram / 首字联想 → 产生下一轮候选
  → ime_userdb 记录词频（脏标记，达到 AUTOSAVE_EVERY 次或显式 flush 才落盘）
```

### 3.3 九键输入

```
按键 PQRS(7) → 数字缓冲 "7"
  → ime_t9_gen("7") → 候选拼音串（Trie 切分）
  → 点击候选拼音 "xi" → 当作全拼缓冲  进入 3.1 流程
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
| P5 | 用户词典脏标记 | 上游 `userdict.cpp` 本来就只在 `close_dict()` 写回；S5 在此之上叠加节流策略 |
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
- **上游完全没有双拼支持**（无 `SetDpScheme`、无方案表），S5 需在
  `src/core/ime_scheme.c` 自行实现方案映射后再喂全拼给引擎。

## 5. 哈希表与字典树的落点

| 结构 | 用途 | 实现说明 |
| --- | --- | --- |
| 哈希表 `ime_hash.c` | 扩展词库索引、S5 的联想 bigram 与用户词频 | 开放寻址 + 线性探测 + 墓碑槽位，容量固定为 2 的幂，键为组件自持副本 |
| 前缀索引 `ime_trie.c` | 九键数字 → 拼音（`94` → `xi`/`yi`/`zi`；`9464` → `xing`/`ying`），最短拼写优先 | 音节表按数字串排序，413 条全表扫描（ESP32-S3 上数微秒），不建节点表、不额外占 RAM |

## 6. 内存预算（ESP32-S3-N16R8，8 MB PSRAM）

| 项 | 规模 | 位置 |
| --- | --- | --- |
| dict 分区映射 | 1.5 MB（实际约 1.1 MB 词库） | flash → mmap，零拷贝优先 |
| 候选 / 会话缓冲 | < 64 KB | 内部 RAM |
| 用户词典（FAT） | 1 MB 分区 | flash |
| 字体分区映射 | ≤ 3 MB | flash → mmap |
| 引擎运行时（Trie 等） | 约 1.5–2 MB（零拷贝路径下更低） | PSRAM 优先 |

## 7. 相关文档

- 键位与布局：[keymap.md](./keymap.md)
- 组件接入：[integration.md](./integration.md)
- 词库二进制格式：[dict_format.md](./dict_format.md)