# 词库与字体二进制格式规范

所有整数均为**小端**（little-endian）。字符串一律为 UTF-8（系统词库除外，见第 3 节）。

---

## 1. 扩展词库 `ime_ext.bin`

SD 卡可选扩展词库，由 `tools/gen_extdict.py` 从文本词表生成。

### 1.1 文件头（16 字节）

| 偏移 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | char[4] | `magic` | 固定 `"IMED"`（0x49 0x4D 0x45 0x44） |
| 4 | u16 | `version` | 当前 `1` |
| 6 | u16 | `reserved` | 固定 `0` |
| 8 | u32 | `record_count` | 记录条数 |
| 12 | u32 | `total_size` | 整个文件字节数（含文件头） |

### 1.2 记录（紧接文件头，连续排列）

```
u8  pinyin_len     拼音 ASCII 字节数（不含 '\0'，音节以 ' 分隔，如 "pi'ao"）
u8  pinyin[]       拼音，ASCII，非 NUL 结尾
u8  word_len       词条 UTF-8 字节数
u8  word_utf8[]    词条，UTF-8
u32 freq           词频（用于候选排序权重）
```

约束：

- `pinyin_len` ≤ 255，`word_len` ≤ 255
- `pinyin` 只允许 `a`–`z` 与 `'`
- 记录按 `pinyin` 的字典序排列，便于前缀检索与流式装载
- 解析端必须校验 `magic`、`version`、以及 `12 + 实际累计字节 == total_size`，
  任一不符即整体拒绝装载（不部分加载）

### 1.3 文本词表输入格式（`data/ext/ext_wordlist_sample.txt`）

```
# 注释行以 # 开头
pinyin<TAB>词条[<TAB>词频]
```

示例：

```
pi'ao	皮袄	120
wo'men	我们	3000
```

## 2. 联想 bigram `ime_bigram.bin`

由 `tools/gen_bigram.py` 从语料生成；未提供语料时不生成，联想降级为"首字联想 + 用户近期词"。

### 2.1 文件头（16 字节）

| 偏移 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | char[4] | `magic` | 固定 `"IMBG"` |
| 4 | u16 | `version` | 当前 `1` |
| 6 | u16 | `reserved` | 固定 `0` |
| 8 | u32 | `pair_count` | 二元组条数 |
| 12 | u32 | `total_size` | 整个文件字节数 |

### 2.2 二元组记录

```
u16  head_len     前词 UTF-8 字节数
u8   head[]       前词，UTF-8
u16  tail_len     后词 UTF-8 字节数
u8   tail[]       后词，UTF-8
u32  freq         共现次数
```

装载后进入开放寻址哈希表（`ime_hash.c`）：key = (head, tail)，value = freq。
表容量取 `next_pow2(pair_count * 2)`，负载因子 ≤ 0.5。

## 3. 系统词库 `dict_pinyin.dat`

沿用 libgooglepinyin 上游二进制格式，**不改动**：

- 来源：`libgooglepinyin-0.1.2.tar.bz2` 的 `data/dict_pinyin.dat`（约 1.1–1.3 MB）
- 获取：`tools/fetch_dict.py`，下载后校验大小与文件头魔数
- 装载方式：
  - `PARTITION`：dict 分区 mmap → 只读 VFS `/dict/dict_pinyin.dat` → 上游 `fopen`
  - `SDCARD` / `EMBEDDED`：直接给出路径 / 内存指针
- 上游 `im_open_decoder_fd(sys_fd, start_offset, length, fn_usr_dict)` 支持从已打开的
  fd + 偏移装载，配合 mmap 可省一次拷贝

## 4. 用户词典分区

- 分区名：`CONFIG_LV_PINYIN_IME_USER_PARTITION`（默认 `usr`）
- 类型：`data, fat` + 磨损均衡，挂载点 `/usr`
- 落盘策略：脏标记 + 显式 flush；每 `CONFIG_LV_PINYIN_IME_USER_AUTOSAVE_EVERY`（默认 10）
  次上屏自动 flush 一次

## 5. 字体分区镜像 `font_partition.bin`

由 `tools/gen_font.py` 生成，采用"两段式 + 目录"结构：

| 偏移 | 内容 |
| --- | --- |
| 0 | 目录：u32 段数 `n`，随后 `n` 组 `{u32 offset, u32 size}` |
| 对齐到 4 字节后 | 段 0：`lv_font_ime_20.bin` |
| 紧接 | 段 1：`lv_font_ime_16.bin` |

- 单段由 `lv_font_conv` 产出：`--format bin --no-compress --bpp 4`
- 目标字符集：GB2312 全集（6763 字）+ `gen_charset.py --extra-scan` 扫描出的补充字
- 运行时：`esp_partition_mmap()` 整段映射，按目录偏移调
  `lv_binfont_create_from_buffer(ptr + offset, size)`
- 预期体积：4bpp/20px 约 1.4 MB；可降级 2bpp（约 700 KB）/ 1bpp（约 350 KB）

配套产出 `generated/font_layout.json` 记录各段的 offset/size，供固件侧或宿主工具核对。

## 6. 校验清单

| 检查项 | 位置 |
| --- | --- |
| `ime_ext.bin` 魔数 / 版本 / 长度一致性 | `ime_extdict.c` 装载入口 |
| `ime_bigram.bin` 同上 | `ime_assoc.c` 装载入口 |
| `dict_pinyin.dat` 大小与魔数 | `fetch_dict.py` + `ime_dict.c` |
| 字体分区目录段数与偏移越界 | `ime_font.c` |

任一校验失败：记录 `ESP_LOGW`/`ESP_LOGE`，进入对应的降级路径（扩展词库/联想/字体），
不得导致启动失败。