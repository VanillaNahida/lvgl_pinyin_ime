# 语音输入（讯飞语音听写 · 流式版）

给键盘加一个语音键，按住或点一下就能把说的话变成文字进输入框。识别在云端做，
用的是讯飞开放平台的[语音听写（流式版）WebAPI](https://www.xfyun.cn/doc/asr/voicedictation/API.html)。

- 开关：`CONFIG_LV_PINYIN_IME_VOICE_ENABLE`（默认 **关**）
- 公开头：`lvgl_pinyin_ime/lv_pinyin_ime_voice.h`
- 实现：`src/voice/`，协议层自带测试 `test_apps/host_voice_test`
- 示例接法：`examples/k26_k9_demo`（Wi-Fi + SNTP + WebSocket + 麦克风骨架）

---

## 1. 分工：组件做协议，调用方做网线和麦克风

组件**不碰网络、不碰 I2S**。它只做讯飞协议，另外两件事由应用通过回调提供：

```
┌──────────────── lvgl_pinyin_ime（组件）────────────────┐
│ 鉴权签名（HMAC-SHA256）· JSON 请求帧 · 返回结果解析     │
│ 动态修正拼接 · 40 ms 节拍 · 状态机与超时                │
└───────▲───────────────────────────────────┬────────────┘
        │ audio_read()  麦克风 PCM           │ ws_open / ws_send_text / ws_close
        │                                   ▼
┌───────┴────────────────────────────────────────────────┐
│ 应用：esp_websocket_client（或别的 ws 库）+ I2S 麦克风   │
│       收到文本帧 → lv_pinyin_ime_voice_ws_message()      │
└────────────────────────────────────────────────────────┘
```

这么切有三个实在的好处：

1. **组件的依赖表没变**（仍然只有 `lvgl` 和 `libgooglepinyin`）。把 esp-tls / Wi-Fi /
   I2S 拉进来，所有接入方都得跟着带。
2. **换东西不用改组件**：换 ws 库、换 codec、换云服务商都只动应用。
3. **协议层能在 PC 上测完**：不需要网络也不需要板子，见第 7 节。

代价是接入方要写 4 个回调 —— 示例工程里已经写好了，照抄即可。

## 2. 五分钟接入

### 2.1 打开开关并填密钥

```powershell
cd examples/k26_k9_demo        # 或你自己的工程
idf.py menuconfig
```

进入 `LVGL Pinyin IME → Voice input (iFlytek IAT)`：

| 配置项 | 说明 |
| --- | --- |
| `Enable voice input` | 总开关 |
| `AppID` / `APIKey` / `APISecret` | 讯飞控制台「语音听写（流式版）」服务页里的三元组 |
| `Endpoint URL` | 默认 `wss://iat-api.xfyun.cn/v2/iat`；小语种换 `iat-niche-api` |
| `Language` / `Domain` / `Accent` | 默认 `zh_cn` / `iat` / `mandarin` |
| `Sample rate` | 8000 或 16000，**必须与麦克风实际采样率一致** |
| `End-of-speech silence` | 静默多久算说完（默认 2000 ms） |
| `Dynamic correction` | 动态修正（边说边出字），默认开 |
| `Show interim text while listening` | 中间结果实时进输入框，默认开 |
| `How the final result enters the text area` | 追加 / 替换本次预览 |
| `Voice key behaviour` | 短按切换 / 长按对讲 / 两者都要（默认两者都要） |

> ⚠ **密钥会随固件发布**，拿到固件的人能提取出来。开发期填这里没问题；量产请留空，
> 在运行时从 NVS 或你自己的服务器取，再用 `lv_pinyin_ime_voice_set_config()` 设进去
> （见第 4.3 节）。

### 2.2 写 4 个回调

```c
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

static int ws_open(void *user, const char *url)   { /* 连 wss，握手完成后才返回 */ }
static int ws_send(void *user, const char *json)  { /* 发一帧文本 */ }
static void ws_close(void *user)                  { /* 关连接，要能容忍"还没连上就关" */ }
static int mic_read(void *user, void *buf, size_t len, uint32_t timeout_ms)
{
    /* 阻塞读 I2S，返回实际字节数；**负数表示硬件错误**。允许阻塞到 timeout_ms。 */
}

static const lv_pinyin_ime_voice_host_t s_host = {
    .user = NULL, .ws_open = ws_open, .ws_send_text = ws_send,
    .ws_close = ws_close, .audio_read = mic_read,
};

/* ws 收到文本帧（在**网络任务**里调用，不必是 LVGL 线程） */
void my_ws_event(const char *data, size_t len)
{
    lv_pinyin_ime_voice_ws_message(s_ime, data, len);
}
```

### 2.3 接到键盘上

```c
lv_obj_t *ime = lv_pinyin_ime_create(lv_screen_active());
lv_pinyin_ime_attach(ime, ta);

lv_pinyin_ime_voice_t *v = lv_pinyin_ime_voice_attach(ime);   /* 键盘上的语音键生效 */
lv_pinyin_ime_voice_cfg_t cfg = lv_pinyin_ime_voice_cfg_default();
cfg.host = &s_host;                                           /* 补上网线与麦克风 */
lv_pinyin_ime_voice_set_config(v, &cfg);
```

`lv_pinyin_ime_voice_attach()` 在 `lv_pinyin_ime_create()` 里就会自动调一次，所以
"先建键盘、后配网络"和"先配网络、后建键盘"都能工作。

### 2.4 别忘了对时

讯飞会校验请求里的时间戳，偏差超过 **300 秒**直接返回 403。所以第一次识别之前必须
跑一次 SNTP：

```c
esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
esp_sntp_setservername(0, "pool.ntp.org");
esp_sntp_init();
/* 等 sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED 再让用户按语音键 */
```

没对时时组件不会硬发请求：状态条会显示「系统时间未同步」，错误码是
`LV_PINYIN_IME_VOICE_ERR_TIME`。

## 3. 键盘上的表现

- **26 键**第 4 行：`?123 | ⌨ | 空格 | 麦克风 | 中/英 | ✓`（空格从 4u 收到 3u，
  两个角键各收 0.5u，整行仍是 12u，列仍与字母行对齐）
- **9 键**第 4 行：`选拼音 | ⌨ | 麦克风 | 空格`（`CONFIG_LV_PINYIN_IME_VOICE_KEY_K9=n` 可关）
- 键面用 `LV_SYMBOL_AUDIO`（FontAwesome 私用区，取自 LVGL 自带符号字体，和 ⌫ / ⏎ 一样）
- 识别期间**候选栏换成状态文案**：`聆听中` → `识别中` → `完成`／错误原因（停留 1.5 s）
- 控件上沿右侧有一个**时长浮窗**（`3.2s`），和拼音浮窗同样式、分居两侧
- 中间结果**实时写进输入框**（`Show interim text while listening`），最终结果替换它

三种按键语义（menuconfig 选）：

| 模式 | 短按 | 长按 |
| --- | --- | --- |
| `TOGGLE` | 开始 / 再按结束 | 结束 |
| `PTT` | 开始 | 松手结束 |
| `BOTH`（默认） | 开始 / 再按结束 | 说完松手结束 |

结束时会发结束帧、等最后一块结果（最多 6 s），拿到就上屏；等服务端自己按静默断句也
可以 —— 两种都会走到同一个收尾路径。

## 4. 公开 API

全部在 `include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h`。

### 4.1 生命周期

```c
lv_pinyin_ime_voice_t *v = lv_pinyin_ime_voice_create(&cfg);  /* NULL = 用 Kconfig 默认 */
lv_pinyin_ime_voice_destroy(v);                              /* 先停任务，再释放 */
```

`lv_pinyin_ime_voice_attach()` 建的会话由键盘控件持有，随控件销毁一起释放，**不要**
自己 destroy 它（要拿它调 API 就用 `lv_pinyin_ime_voice_of(ime)`）。

### 4.2 控制与查询

| 函数 | 作用 |
| --- | --- |
| `lv_pinyin_ime_voice_start(v)` | 开始识别（非阻塞）；返回 0 或负的错误码 |
| `lv_pinyin_ime_voice_stop(v)` | 结束并等最终结果上屏（非阻塞） |
| `lv_pinyin_ime_voice_cancel(v)` | 取消，**连已识别的部分也丢掉** |
| `lv_pinyin_ime_voice_state(v)` | 当前状态（IDLE/CONNECTING/LISTENING/FINISHING/DONE/ERROR） |
| `lv_pinyin_ime_voice_error(v)` | 错误原因 |
| `lv_pinyin_ime_voice_server_code(v)` | 服务端错误码（对着文档错误码表查） |
| `lv_pinyin_ime_voice_state_text(v)` | 状态的可读文案，可直接贴到 UI |
| `lv_pinyin_ime_voice_text(v)` | 当前已识别的文本（UTF-8 预览） |
| `lv_pinyin_ime_voice_audio_ms(v)` | 已发送的音频毫秒数（排查"只发了几帧"很有用） |

`state()` / `text()` / `error()` 可以在任意线程读（只是读字段）。

### 4.3 运行时改配置（放密钥的正路）

```c
lv_pinyin_ime_voice_cfg_t cfg = lv_pinyin_ime_voice_cfg_default();  /* 取 Kconfig 的端点/语种 */
cfg.host = &s_host;
/* 从 NVS 或服务器取密钥，填进 cfg.appid/apikey/apisecret */
lv_pinyin_ime_voice_set_config(v, &cfg);
```

会话进行中改配置不影响当前这一次，下次 `start` 生效。键盘还会在每次开始时把
`language` 同步成当前键盘语言（中文键盘 → `zh_cn`，英文键盘 → `en_us`），其余字段
保持你设的值。

### 4.4 事件

```c
lv_obj_add_event_cb(ime, on_voice_state, lv_pinyin_ime_event_voice_state(), NULL);
lv_obj_add_event_cb(ime, on_voice_result, lv_pinyin_ime_event_voice_result(), NULL);
```

`voice_result` 在最终文本**已经写进输入框**之后抛出，适合做提示音、自动发送之类。

## 5. 音频要求（最容易出错的地方）

| 项 | 要求 |
| --- | --- |
| 采样率 | 16000 或 8000，且**与 `Sample rate` 配置一致** |
| 位宽 | 16 bit |
| 声道 | 单声道 |
| 字节序 | 小端 |
| 每帧 | 16k → 1280 B（40 ms）；8k → 640 B。由组件按 40 ms 节拍调用 `audio_read()` |

`audio_read()` 可以阻塞到 `timeout_ms`：I2S 读本来就是阻塞的，直接读就行，**不要**在
里面另起线程或自己攒缓冲 —— 40 ms 的节拍与分帧由组件负责。返回 0 表示这一拍没数据
（当静音处理，不报错）；返回负数表示硬件错误，连续 5 次才中断本次识别（偶发抖动不
该打断整段话）。

### 5.1 讯飞只收 16 k / 8 k，重采样必须由调用方做

接口**不支持**原始采样率：`data.format` 只接受 `audio/L16;rate=16000` 或 `rate=8000`，
没有"按麦克风实际采样率上传"这种用法。所以板子上的 codec 跑 48 kHz（很常见）时，调用方
必须先自己降到 16 k 单声道再交给组件。典型链路：

```
codec 48 kHz stereo ──┬─ 取一路声道（或两路平均）
                      ├─ 抗混叠低通（截止 ~7.6 kHz）← 最容易漏的一步
                      └─ 抽取到 16 kHz ─→ audio_read() 返回 16 k 单声道 PCM
```

两个实践要点：

- **别只做抽取、不做滤波**。48 k → 16 k 是 3 倍抽取，直接每 3 个样本取 1 个会把
  8 kHz 以上的能量**折叠**到可听频带里，识别率明显下降（听感上是"沙沙"的金属音）。
  要么做低通，要么用带抗混叠的库（`esp_audio_codec` / `speex` 的重采样器）。
- **重采样放在你的函数里，不要在 `audio_read()` 里临时开大数组**。这个坑和 §8.4 里
  组件踩的是同一个：重采样器常常要几百字节到几 KB 的中间缓冲，放在任务栈上就会
  Stack protection fault —— 而且崩在**你的**任务里，日志看着像语音组件的锅。

## 6. 出错时看到什么

状态条上的文案与错误码一一对应：

| 文案 | 错误码 | 常见原因 |
| --- | --- | --- |
| 语音未配置密钥 | `ERR_NOT_CONFIGURED` | menuconfig 没填三元组 |
| 语音未接网络接口 | `ERR_NO_HOST` | 没设 `cfg.host` |
| 系统时间未同步 | `ERR_TIME` | 没跑 SNTP，或对时还没完成 |
| 语音连接失败 | `ERR_WS_OPEN` | 网络不通 / 证书 / 403（见下） |
| 语音发送失败 | `ERR_WS_SEND` | 连接被中途关闭 |
| 麦克风读取失败 | `ERR_AUDIO` | I2S 没初始化或缺时钟 |
| 语音连接已断开 | `ERR_WS_CLOSED` | 对端在最终结果之前断开 |
| 语音识别超时 | `ERR_TIMEOUT` | 6 s 内没等到最终结果，且一个字都没识别出来 |
| 语音错误 N | `ERR_SERVER` | 服务端返回非 0 code，N 就是文档里的错误码 |

握手阶段最常见的两个服务端拒绝（HTTP 401/403，属于 `ERR_WS_OPEN`）：

- `HMAC signature does not match` —— APIKey/APISecret 不对
- `a valid date or x-date header is required` —— 时钟偏差超 300 s

## 7. 验证

### 7.1 协议层（主机端，不需要网络与硬件）

```powershell
cd <仓库根>
gcc -std=c11 -Wall -Wextra -Werror -Isrc `
  test_apps/host_voice_test/main.c `
  src/voice/ime_voice_proto.c src/voice/ime_voice_crypto.c `
  -o build/host_voice_test/host_voice_test.exe
.\build\host_voice_test\host_voice_test.exe
```

也可以照旧用 CMake（`cmake -S test_apps/host_voice_test -B build/host_voice_test -G Ninja`）。
136 条断言，覆盖：

- **HMAC-SHA256**：RFC 4231 的用例 1/2/6（含密钥超块长那条分支）
- **鉴权 URL**：用文档给的时间戳与 `APISecret`，**逐字节**比对文档里的
  `signature` 与 `authorization`
- **base64**：RFC 4648 向量、0..255 全字节往返、缓冲不足要拒绝、非法字符要拒绝
- **请求帧**：首帧带 `common`+`business`、中间帧只带 `data`、结束帧 `{"data":{"status":2}}`、
  1280 B → 1708 字符 base64 且解回来一致、缓冲不足要拒绝
- **返回帧解析**：把文档里的样例原样喂进去（动态修正 `pgs/rpl` + `rg`、`vinfo` 的
  `vad`、句子多候选 `nbest`、词级多候选 `wbest`、错误帧、数字写成字符串）
- **动态修正段落表**：`apd` 追加、`rpl` 替换（含 `rg=[1,2]` 合并）、“重复同一帧应报告
  无变化”、`sn/rg` 越界要钳位
- **流式性**：用官方 demo 带的 `iat_pcm_16k.pcm`（152 916 B）走完整帧序，断言
  **每收一帧就能拿到当前文本**，并且收尾帧不会把最后一段重复一遍
- **字形过滤**：emoji / 非法 UTF-8 要丢掉、缓冲不足要截断而不越界

> 其中"`vad.bg` 与 `ws[].bg` 层级不能串"那条是有来历的：返回帧里两个层级都有 `bg`，
> 早期用 `strstr("\"bg\"")` 找键，命中哪个取决于服务端的字段顺序 —— 数值错了却不报错。
> 现在解析器按 JSON 结构走（`json_next` 逐层取键、数组用 `json_elem`），测试直接把
> 文档的 `vinfo` 样例钉住了。

### 7.2 编译校验（两种配置）

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_voice_build.ps1
```

用**现有 IDF 构建目录里的编译参数**（同一个交叉编译器、同一套 include）对语音模块做
`-fsyntax-only`，跑两遍：

- `voice ON`：`src/voice/*` 四个文件 + `src/ui/` 三个文件
- `voice OFF`（默认配置）：只编 `src/ui/` 三个文件

第二遍不能省：默认配置就是"语音关"，此时 `src/ui/` 里所有
`#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE` 会走另一条分支，而那条路才是绝大多数接入方
实际编到的代码。两种都编才不会出现"开了语音能编、默认配置编不过"。

脚本有两处刻意做成"从真实来源推导"，都是为了不再出现"检查通过、真构建失败"：

- **配置头是生成的**：从 `examples/k26_k9_demo/build/config/sdkconfig.h` 读进来
  （去掉语音与示例相关的行），再叠加"语音打开"的一组值，写到
  `test_apps/idf_component_build/voice_check_config.h`。手写常量等于给同一批值
  造第二个来源，会和真实构建撞车（bool/int 重复定义直接 `-Werror` 报错、
  字符串因内容相同被静默接受），症状是"只有部分文件失败"，很难查。
- **include 目录是从组件 CMakeLists.txt 里读的**：曾经这里手写死了 `src/ui`，
  而 CMakeLists 里漏了它 —— 于是检查全过、`idf.py build` 一开语音就报
  `lv_pinyin_ime_internal.h: No such file or directory`。

脚本要求 `examples/k26_k9_demo/build/compile_commands.json` 存在（先跑过一次
`idf.py build`）。它不重新配置 cmake，所以几秒钟就出结果。

### 7.3 真机

照 `docs/integration.md` §7 的标准流程构建三个目标：

```powershell
& "D:\Espressif\EIM\v6.1\esp-idf\export.ps1"
cd examples\k26_k9_demo
idf.py set-target esp32s3 ; idf.py build
idf.py set-target esp32p4 ; idf.py build
```

**组件侧**已验证：默认配置（语音关）的 `idf.py build` 在 esp32s3 上输出
`Project build complete`；打开语音开关后 `src/voice/` 的四个文件都编译通过
（`liblvgl_pinyin_ime.a` 正常产出），协议层 136 条断言全过。

**示例工程的语音接线**（`main/net_wifi.c` / `net_ws.c` / `mic_i2s.c`）**尚未**在
语音打开的情况下整工程构建通过，见 §10 开头的说明。

## 8. 实现要点（改代码前先看这里）

### 8.1 线程模型

会话有一个自己的 FreeRTOS 任务，**所有会话状态的推进都在它里面**：

```
LVGL 线程                    组件任务                     应用网络任务
  start/stop/cancel  ──────▶  每 40 ms:
  读 state()/text()            audio_read → 拼帧 → ws_send_text
  收状态回调  ◀──────────────  set_state() / result_cb
                               ▲
                               └── 队列 ◀── ws_message()（只入队，不碰 LVGL）
```

- 网络回调 `lv_pinyin_ime_voice_ws_message()` **只把内容拷进队列**，绝不碰 LVGL。
- 组件任务想更新界面时用 `lv_async_call()` 唤醒 LVGL 线程；LVGL 9 的这个调用**不会
  排队重复的回调**（同一函数 + 同一 user_data 只留一个），所以状态连续变化不会堆积。
- 界面侧另有一个 100 ms 的 `lv_timer` 读会话状态 —— 回调只能告诉 LVGL"有事发生"，
  真正的状态值总要有人读，顺手也用它处理"完成/错误文案停留 1.5 s 后交还候选栏"。

### 8.2 为什么自己写 JSON 解析而不引 cJSON

要解析的结构是**固定**的（只有 `code/message/sid/data.result.{sn,ls,pgs,rg,vad,ws[].cw[].w}`），
手写一个够用的解析器比让所有接入方多背一个依赖更可控。解析器有三个约定：

1. **先就地解 `\uXXXX`**：服务端会用 `\u` 转义汉字，就地解码不会让字符串变长
   （6 字节 → 最多 4 字节），之后全按普通 UTF-8 处理。
2. **解析边界必须显式传**（`json_match(p, open, end)`）：调用方给的 buffer 只保证
   "长度 len 之内是 JSON"，len 之后是什么完全未知 —— 括号匹配若扫到 NUL 为止，就会
   读到别人的内存。这是踩过的坑：`buf[len] = '\0'` 只保证第一个字节，`json_match`
   却会一直往后扫，于是解析结果取决于栈上残留的垃圾。
3. **对象用 `json_next`，数组用 `json_elem`**：`json_next` 会跳过 `{` 去找键名，对
   `[{"bg":7}]` 它会把里面的 `bg` 当成成员返回 —— 元素这一层就丢了。两个必须分开。

### 8.3 会话时长

文档说整个会话最长 60 s，所以组件在 **55 s** 主动发结束帧（`IME_VOICE_MAX_SESSION_MS`），
留 5 s 给最后一块结果的往返。与其让用户撞上"识别突然失败"，不如先把结果拿到手。

### 8.4 任务栈：踩过三次，全是"以为够，其实差一个数量级"

这部分是**真机崩过三次**之后补的，改动前请先读完。

#### ⚠ 第一件必须先记住的事：`xTaskCreate` 的深度单位是**字节**，不是字

ESP-IDF 的 FreeRTOS 端口里：

```c
/* FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h */
#define portSTACK_TYPE              uint8_t
typedef portSTACK_TYPE              StackType_t;
```

RISC-V 与 Xtensa 两个端口都是 `uint8_t`。所以 `xTaskCreate(..., 1536, ...)` 给的是
**1536 字节**，不是 6 KB —— 按"字"（4 字节）理解会差 4 倍，而编译器不会提醒你。

验证方法：崩溃日志里任务的 `Stack bounds` 直接给出真实大小。曾经写下 1536、以为等于
6 KB，日志里是 `0x2f04310c - 0x2f043700` = **1524 字节**，一眼就对上了。

`uxTaskGetStackHighWaterMark()` 的单位是 `StackType_t` 个数，在这个端口上也就是字节数
（代码里仍写 `hwm * sizeof(StackType_t)`，是为了在别的端口上也正确）。

#### 三次崩溃的时间线

| # | 栈 | 崩在哪 | 根因 |
| --- | --- | --- | --- |
| 1 | 4096 | `ime_voice_build_auth_url` | 该函数有 6 个局部数组，约 2.3 KB |
| 2 | 8192 | `handle_reply` | `ime_voice_reply_t r;` —— **这个结构体单独 12 744 字节**（`segs[32] × 392 B`），栈开多大都不该放它 |
| 3 | **1536** | `ime_voice_build_auth_url` 又崩 | 把 6144 当"字"改成 1536，实际只给了 1536 **字节**；同一个 2.3 KB 的函数立刻又爆 |

第 3 次最值得记：**同一个函数崩了两次**。第一次以为"栈太小"（于是把栈调大），第二次才
看清是"这个函数在栈上放了 2.3 KB 的东西"。所以正确的修法是**搬缓冲**，不是调栈。

#### 现在的做法：所有大缓冲都在会话工作区（堆）

`ime_voice_scratch_t`，随会话 `malloc` 一次、任务循环反复复用：

| 内容 | 大小 | 曾经在哪 |
| --- | --- | --- |
| `ime_voice_reply_t reply` | 12 744 B | `handle_reply()` 的局部变量 |
| `auth`（`ime_voice_auth_scratch_t`） | 2 336 B | `build_auth_url()` 的 6 个局部数组 |
| 拼帧缓冲 `frame` | 3 200 B | `voice_task()` 的局部数组 |
| PCM 帧 `pcm` | 1 280 B | 同上 |
| 整句快照 `before_text` | 640 B | `transcript_apply()` 的局部数组 |
| **合计** | **≈ 20 KB** | 全在堆上 |

搬完之后的实测栈帧（`-fstack-usage`，`tools/check_stack_usage.ps1`）：

| 路径 | 栈深 |
| --- | --- |
| 签名 `build_auth_url_now` 80 → `build_auth_url` **96** → `hmac_sha256` 368 → `sha256_block` 352 | ≈ **1008 B**（最坏） |
| 收帧 `handle_reply` 112 → `parse_reply` 80 → `parse_result` 144 → `parse_seg` 96 | ≈ 560 B |

`build_auth_url` 从 **2464 B → 96 B**，这就是"搬缓冲"的效果。任务栈定为
`IME_VOICE_TASK_STACK = 8192`（字节），相对最坏链约 8 倍余量，用来吸收
`ESP_LOG` 的 `snprintf`、IDF 初始化等不在我们 `.su` 统计里的调用。

#### 下次再出这类问题的排查顺序

```powershell
# 1. 量栈帧（交叉编译器，几秒钟）
powershell -ExecutionPolicy Bypass -File tools\check_stack_usage.ps1
#    列出每个函数的帧大小并和任务栈对比，余量 < 1 KB 会报警

# 2. 真机每次会话结束会打一行实测余量
#    I (xxxxx) ime_voice: voice task stack free at exit: 7840 B (high-water mark)
#    接近 0 → 又有大家伙混进栈里了，去查局部变量
```

#### 编译期护栏

`ime_voice.c` 里有几条 `_Static_assert` 把这几件事钉住：

- 工作区必须含 `reply` 与 `auth` 两块（谁挪回局部变量，结构体就会变小 → 断言失败）
- `IME_VOICE_TASK_STACK >= 4096`（防止再有人按"字"把它写成一千多）

### 8.5 字形过滤

识别结果来自云端，字表外的字会画成缺字方框。所以文本进输入框之前要过一道筛子，
判据是**当前字体到底有没有这个字形**（`lv_font_get_glyph_dsc()` + `is_placeholder`，
候选栏的翻页箭头早就在用这招）。这样应用换更全的字体（`lv_pinyin_ime_set_fonts()`）
时自动放宽，不需要改代码，也不用维护第二份字表。

## 9. 已知限制

- **一次会话最多 60 s**（服务端限制）。到 55 s 组件会自动收尾上屏，想继续说话要再点
  一次语音键。
- **动态修正仅中文普通话**支持，且与 `vinfo` 互斥 —— 组件只开 `wpgs`。
- **不做本地 VAD**：断句完全交给服务端的 `eos`。好处是实现简单、行为一致；代价是
  完全静音时也要等 `eos` 那么久才出最终结果。
- **密钥在固件里会被提取**（见 §2.1 的警告）。
- 示例工程的麦克风部分是**桩**，见下一节。

## 10. 示例工程里要你补的东西

> **状态：搁置，未完成真机验证。** 下面这套接线（Wi-Fi + SNTP + WebSocket + 麦克风）
> 已经写完、也通过了 `tools/check_voice_example.ps1` 的编译检查（用真 IDF 头 +
> 一个 `esp_websocket_client` 桩头），但**还没有在打开语音开关的情况下整工程构建
> 通过**。已知要处理的两件事：
>
> 1. **配置缓存**：`main/CMakeLists.txt` 的语音分支会给 `main` 追加 `REQUIRES`
>    （`esp_driver_i2s` / `esp_netif` / `esp_event` / `esp_wifi`）。改完必须让 CMake
>    重新配置，否则新加的 include 目录不生效，症状是
>    `driver/i2s_pdm.h: No such file or directory` 与 `esp_netif.h: No such file or
>    directory`：
>
>    ```powershell
>    idf.py reconfigure     # 或 idf.py fullclean && idf.py build
>    ```
>
> 2. **`esp_websocket_client` 的依赖声明**：`main/idf_component.yml` 里现在是
>    **无条件**依赖（`espressif/esp_websocket_client: "^1.5"`）。曾经用过带
>    `rules: if CONFIG_...` 的条件依赖，实测组件管理器会打印
>    `NOTE: Skipping optional dependency` 并真的不下载，于是 `net_ws.c` 直接
>    `fatal error: esp_websocket_client.h`。无条件写法的代价是"语音关掉时也会下载
>    这个组件"（下到 `managed_components/`，不编进固件），换来的是不会莫名其妙
>    编不过。要继续用条件依赖的话，请先确认组件管理器真的拉下来了。

`examples/k26_k9_demo` 提供了 Wi-Fi + SNTP + WebSocket 的实现与麦克风桩。
参考板（ESP32_MusicPlayer_V4）的音频硬件信息不在本仓库里，所以 `main/mic_i2s.c`
留了 4 个 TODO：

1. `MIC_I2S_PORT` / `MIC_PIN_BCLK` / `MIC_PIN_WS` / `MIC_PIN_DIN` —— 按你的板子接线
2. 如果是 I2S codec（ES8311 之类）而不是裸 MEMS 麦克风：用 `i2s_std` + codec 驱动
   替换 `i2s_pdm` 配置，并确认 codec 的采样率/位宽与 menuconfig 一致
3. `mic_i2s_read()` 里的增益／声道选择（PDM 立体声下左右声道可能只有一边有信号）
4. 若要"说话时先清空麦克风 DMA 里的旧数据"，在 `audio_read` 回调首次调用时读掉几帧

Wi-Fi 的 SSID/密码也在 `sdkconfig` 里（`CONFIG_EXAMPLE_WIFI_SSID` / `_PASSWORD`），
示例用的是专门的 Kconfig 选项而不是把密码写进源码。
