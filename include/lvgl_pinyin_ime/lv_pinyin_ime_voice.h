/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 语音输入（讯飞语音听写 · 流式版）
 *
 * 这一层做的是**讯飞协议**：鉴权签名、40 ms 一帧的音频推送、返回结果解析、
 * 动态修正拼接、状态机与超时。它**自己不做两件事**：
 *
 *   1. 网络 —— WebSocket 由调用方建立与收发（见 lv_pinyin_ime_voice_host_t）
 *   2. 采集 —— 麦克风 PCM 由调用方提供（同上）
 *
 * 为什么这么切：组件至今只依赖 lvgl 与 libgooglepinyin。一旦把 esp-tls / Wi-Fi /
 * I2S 拉进来，所有接入方都被迫跟着带这些依赖，换芯片、换 codec、换云服务商都要
 * 改组件。切成回调之后，组件是"协议客户端"，调用方是"网线 + 麦克风"：
 *
 *   ┌────────── lvgl_pinyin_ime ──────────┐        ┌──── 调用方（app）────┐
 *   │ 鉴权URL / JSON 帧 / 结果解析 / 状态机 │ ──────▶│ ws_open/send/close   │
 *   │ 40 ms 节拍 / 动态修正 / 超时          │ ◀──────│ audio_read（麦克风） │
 *   └──────────────────────────────────────┘        └──────────────────────┘
 *
 * 调用方要做的（示例工程 examples/k26_k9_demo 里有骨架）：
 *   - 连上网络并 SNTP 对时（讯飞校验 date 偏移 ≤ 300 s，差太多会 403）
 *   - 用 esp_websocket_client 之类的库实现 4 个回调
 *   - 从 I2S 麦克风读出 16 kHz / 16 bit / 单声道 PCM
 *   - ws 收到文本帧时把内容交给 lv_pinyin_ime_voice_ws_message()
 * ========================================================================== */

/* ------------------------------------------------------------------ 状态 */

/** 语音会话状态。 */
typedef enum {
    LV_PINYIN_IME_VOICE_IDLE = 0, /**< 未在识别 */
    LV_PINYIN_IME_VOICE_CONNECTING, /**< 已发起，等 WebSocket 握手 */
    LV_PINYIN_IME_VOICE_LISTENING,  /**< 正在推音频、收结果 */
    LV_PINYIN_IME_VOICE_FINISHING,  /**< 已发结束帧，等最后一块结果 */
    LV_PINYIN_IME_VOICE_DONE,       /**< 成功结束（短暂停留后回到 IDLE） */
    LV_PINYIN_IME_VOICE_ERROR,      /**< 出错结束（短暂停留后回到 IDLE） */
    LV_PINYIN_IME_VOICE_STATE_LAST,
} lv_pinyin_ime_voice_state_t;

/** 出错原因。取值对应讯飞错误码，另有几个是本地判定的。 */
typedef enum {
    LV_PINYIN_IME_VOICE_ERR_NONE = 0,
    LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED, /**< 没配 AppID/APIKey/APISecret */
    LV_PINYIN_IME_VOICE_ERR_NO_HOST,        /**< 没提供 host 回调 */
    LV_PINYIN_IME_VOICE_ERR_TIME,           /**< 系统时间没同步（时钟还没对） */
    LV_PINYIN_IME_VOICE_ERR_WS_OPEN,        /**< ws_open 失败 */
    LV_PINYIN_IME_VOICE_ERR_WS_SEND,        /**< ws_send_text 失败 */
    LV_PINYIN_IME_VOICE_ERR_AUDIO,          /**< audio_read 失败 */
    LV_PINYIN_IME_VOICE_ERR_WS_CLOSED,      /**< 对端提前断开 */
    LV_PINYIN_IME_VOICE_ERR_TIMEOUT,        /**< 等结果超时 */
    LV_PINYIN_IME_VOICE_ERR_SERVER,         /**< 服务端返回非 0 code（见 server_code） */
    LV_PINYIN_IME_VOICE_ERR_BUSY,           /**< 已在识别中，重复 start */
} lv_pinyin_ime_voice_err_t;

/* ------------------------------------------------------------- 调用方接口 */

/**
 * 调用方提供的网络与麦克风实现。
 *
 * 四个函数都会在**组件自己的任务**里被调用（不是 LVGL 线程，也不是中断），所以
 * 内部可以阻塞、可以等待信号量。
 */
typedef struct {
    /** 传给下面每个回调的原样上下文。 */
    void *user;

    /**
     * 建立 WebSocket 连接。
     *
     * @param url 组件签好名的完整 URL（含 authorization/date/host 查询参数），
     *            形如 wss://iat-api.xfyun.cn/v2/iat?authorization=...&date=...&host=...
     *            调用方只管把它交给 ws 客户端，不要改它。
     * @return 0 成功；非 0 失败（状态机转 ERROR 并上报 ERR_WS_OPEN）
     *
     * ★ 要求"握手完成后才返回"：组件紧接着就要发第一帧音频。
     */
    int (*ws_open)(void *user, const char *url);

    /**
     * 发送一帧文本消息（一段完整 JSON）。
     *
     * @param json NUL 结尾的 JSON
     * @return 0 成功；非 0 失败（状态机转 ERROR 并上报 ERR_WS_SEND）
     */
    int (*ws_send_text)(void *user, const char *json);

    /**
     * 关闭连接。
     *
     * 会话正常结束、出错、被取消时都会调用一次，实现方要能容忍"还没连上就关"。
     */
    void (*ws_close)(void *user);

    /**
     * 读一段麦克风 PCM。
     *
     * 格式：**16 kHz（或 8 kHz，与配置的 rate 一致）、16 bit、单声道、小端**。
     * 组件每 40 ms 调一次，期望拿到 ime_voice_frame_bytes(rate) 字节（16k 是
     * 1280 B）。
     *
     * ★ 请**尽量把 @p buf 填满**再返回。读不满时组件会把这一小段当成"一整帧的
     *   40 ms 音频"发出去，结果音频流被拉长、时间轴与实际说话错位，识别会变差
     *   （真机上表现为"说了话却什么都没识别出来"）。如果底层一次只给一小块
     *   （I2S 常见 512 / 1024 B 一块），请在回调内部循环读到满或超时为止。
     *   组件也会尽力兜底（一次读不满会再读几次，见 ime_voice.c），但回调里一次
     *   读满最省事、也最不容易出问题。
     *
     * 返回 0 表示这一拍确实没数据（当静音处理，不报错）。返回负数表示硬件错误
     * （连续 5 次才中断本次识别，偶发抖动不打断）。
     *
     * ★ 允许阻塞到 @p timeout_ms：I2S 读本来就是阻塞的，实现方直接读就行，
     *   不要在回调里起线程或攒缓冲 —— 40 ms 的节拍由组件负责。
     *
     * @param buf        目标缓冲
     * @param len        最多读多少字节
     * @param timeout_ms 最多等多久
     * @return 实际读到的字节数；**负数表示硬件错误**（状态机转 ERROR 并上报
     *         ERR_AUDIO）
     */
    int (*audio_read)(void *user, void *buf, size_t len, uint32_t timeout_ms);
} lv_pinyin_ime_voice_host_t;

/**
 * 会话配置。
 *
 * 用 LV_PINYIN_IME_VOICE_CFG_DEFAULT() 初始化：它填的是 Kconfig（sdkconfig）里的
 * 值，所以"在 menuconfig 里填 AppID/APIKey/APISecret"就够了。
 *
 * ⚠ 密钥会随固件一起发布，拿到固件的人就能提取出来。量产建议 Kconfig 只留端点与
 *   语种，密钥在运行时从 NVS / 服务器下发后再调 lv_pinyin_ime_voice_set_config()。
 */
typedef struct {
    /** 讯飞控制台的 AppID。留空则用 Kconfig。 */
    char appid[40];
    /** 讯飞控制台的 APIKey（32 位）。留空则用 Kconfig。 */
    char apikey[68];
    /** 讯飞控制台的 APISecret（32 位）。留空则用 Kconfig。 */
    char apisecret[68];
    /**
     * 接口地址，不含查询参数。
     *
     * 中英文（推荐）：wss://iat-api.xfyun.cn/v2/iat
     * 小语种：      wss://iat-niche-api.xfyun.cn/v2/iat
     * 留空则用 Kconfig。
     */
    char endpoint[128];
    /** 语种："zh_cn" / "en_us" / ... 留空则用 Kconfig。 */
    char language[16];
    /** 领域：日常用语 "iat"，方言免切 "xfime-mianqie"，医疗 "medical" ... */
    char domain[32];
    /** 方言：普通话 "mandarin"，其它方言需在控制台开通。 */
    char accent[16];

    /** 音频采样率。16 kHz 每帧 1280 B，8 kHz 每帧 640 B。 */
    int sample_rate;
    /** 后端点检测静默时长（毫秒）：静这么多毫秒后服务端认为说完并给最终结果。 */
    int eos_ms;
    /**
     * 动态修正（dwa=wpgs，仅中文普通话）。
     *
     * 开：边说边出字，服务端可以回头**替换**已发的结果（"明天"→"明天天气"），
     *     观感最好。关：每次返回都是对之前的追加，字要等整句说完才准。
     */
    bool dynamic_correction;
    /** 是否让服务端加标点。 */
    bool punctuation;

    /**
     * 是否把识别中的文本实时送进输入框（预览）。
     *
     * 开：边说边出字，最后以最终结果替换预览。
     * 关：只在识别结束时上一次屏。
     */
    bool live_preview;

    /**
     * 最终结果怎么进输入框：
     *   true  —— 追加到光标处（默认）
     *   false —— 先删掉本次会话写入的预览，再追加（"替换"观感）
     */
    bool append_result;

    /** 网络与麦克风实现。必须提供。 */
    const lv_pinyin_ime_voice_host_t *host;
} lv_pinyin_ime_voice_cfg_t;

/**
 * 用 Kconfig（sdkconfig）里的值初始化配置。
 *
 * 没开 CONFIG_LV_PINYIN_IME_VOICE_ENABLE 时这个函数仍然存在，但配置里没有密钥，
 * 直接 start 会得到 ERR_NOT_CONFIGURED —— 这样调用方的代码不用改就能编译。
 */
lv_pinyin_ime_voice_cfg_t lv_pinyin_ime_voice_cfg_default(void);

/* ============================================================================
 * 单次识别会话（不依赖键盘控件）
 *
 * 这一层可以在没有 LVGL 控件的环境里用（比如只想拿识别文本）。键盘把语音键接到
 * 它上面，见本文档末尾的"键盘集成"部分。
 * ========================================================================== */

/** 不透明会话句柄。 */
typedef struct lv_pinyin_ime_voice lv_pinyin_ime_voice_t;

/** 会话状态变化 / 有结果时的回调。**在组件任务里调用**，不要在里面碰 LVGL。 */
typedef void (*lv_pinyin_ime_voice_cb_t)(lv_pinyin_ime_voice_t *v, void *user);

/**
 * 创建会话。
 *
 * @param cfg 配置（会被复制）；NULL 等价于 cfg_default()
 * @return 句柄；内存不足返回 NULL
 */
lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_create(const lv_pinyin_ime_voice_cfg_t *cfg);

/** 销毁会话：先停止识别，再释放。传 NULL 安全。 */
void lv_pinyin_ime_voice_destroy(lv_pinyin_ime_voice_t *v);

/**
 * 就地改配置（密钥从 NVS 读出来后再设、或运行期切语种都用它）。
 *
 * 会话进行中改配置不会影响当前这一次，下次 start 生效。
 */
void lv_pinyin_ime_voice_set_config(lv_pinyin_ime_voice_t *v,
                                    const lv_pinyin_ime_voice_cfg_t *cfg);

/** 取当前配置（只读，不要改）。 */
const lv_pinyin_ime_voice_cfg_t *lv_pinyin_ime_voice_get_config(const lv_pinyin_ime_voice_t *v);

/** 状态变化回调。 */
void lv_pinyin_ime_voice_set_state_cb(lv_pinyin_ime_voice_t *v,
                                      lv_pinyin_ime_voice_cb_t cb, void *user);

/** 最终结果就绪回调（在 cb 里用 lv_pinyin_ime_voice_result() 取文本）。 */
void lv_pinyin_ime_voice_set_result_cb(lv_pinyin_ime_voice_t *v,
                                       lv_pinyin_ime_voice_cb_t cb, void *user);

/**
 * 开始识别。
 *
 * 内部会：对时并签名 → ws_open() → 起任务，每 40 ms 调 audio_read() 并发送一帧。
 *
 * @return 0 成功；负数见 lv_pinyin_ime_voice_err_t（已在识别中返回 -BUSY）
 */
int lv_pinyin_ime_voice_start(lv_pinyin_ime_voice_t *v);

/**
 * 结束识别：发结束帧、等最后一块结果、上屏。
 *
 * 非阻塞（状态转 FINISHING）；服务端返回最终结果或超时后落到 DONE。
 */
void lv_pinyin_ime_voice_stop(lv_pinyin_ime_voice_t *v);

/**
 * 取消识别：断开连接，**不上屏任何东西**（连已经收到的部分也丢掉）。
 */
void lv_pinyin_ime_voice_cancel(lv_pinyin_ime_voice_t *v);

/** 当前状态。 */
lv_pinyin_ime_voice_state_t lv_pinyin_ime_voice_state(const lv_pinyin_ime_voice_t *v);

/** 上一次结束的原因（出错时非 0）。 */
lv_pinyin_ime_voice_err_t lv_pinyin_ime_voice_error(const lv_pinyin_ime_voice_t *v);

/** 服务端返回的错误码（ERR_SERVER 时才有意义，否则 0）。 */
int lv_pinyin_ime_voice_server_code(const lv_pinyin_ime_voice_t *v);

/** 状态的可读文案（"聆听中" / "识别中" / 错误原因），用于状态条。UTF-8。 */
const char *lv_pinyin_ime_voice_state_text(const lv_pinyin_ime_voice_t *v);

/** 当前已识别的文本（识别过程中是中间结果）。UTF-8，可能为空串。 */
const char *lv_pinyin_ime_voice_text(const lv_pinyin_ime_voice_t *v);

/** 最终结果是否已就绪（取走后不会清零；用 text() 拿内容）。 */
bool lv_pinyin_ime_voice_result_ready(const lv_pinyin_ime_voice_t *v);

/** 本次会话已发送的音频毫秒数（用来显示时长 / 排查"只发了几帧"）。 */
uint32_t lv_pinyin_ime_voice_audio_ms(const lv_pinyin_ime_voice_t *v);

/**
 * 网络回调：收到一帧文本消息。
 *
 * ★ 在**调用方自己的网络任务**里调用即可，不必是 LVGL 线程：内部会把内容拷进队列，
 *   由组件任务处理。
 *
 * ★ @p json 之后必须还有一个可写字节：解析是就地做的（把 \uXXXX 解成 UTF-8），
 *   会写 buf[len] = '\0'。大多数 ws 库给的缓冲都满足（esp_websocket_client 的回调
 *   参数就是这样）。
 *
 * @param v    会话；NULL 时忽略
 * @param json 文本帧内容（UTF-8）
 * @param len  字节数（不含结尾 NUL）
 */
void lv_pinyin_ime_voice_ws_message(lv_pinyin_ime_voice_t *v, const char *json, size_t len);

/**
 * 网络回调：连接已断开。
 *
 * @param err 0 表示正常关闭（识别已结束时的关闭），非 0 表示异常断开
 */
void lv_pinyin_ime_voice_ws_closed(lv_pinyin_ime_voice_t *v, int err);

/* ============================================================================
 * 键盘集成
 * ========================================================================== */

/** 语音功能是否已编译进来（Kconfig 开关）。 */
bool lv_pinyin_ime_voice_supported(void);

/**
 * 把语音能力接到键盘控件上：键盘上的语音键、候选栏状态条、输入框预览都由它驱动。
 *
 * @param ime 键盘根对象
 * @return 会话句柄；不支持或失败返回 NULL
 */
lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_attach(lv_obj_t *ime);

/** 取键盘上挂着的语音会话（没接就是 NULL）。 */
lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_of(lv_obj_t *ime);

/**
 * 键盘上的语音键被按下（短按）。
 *
 * 行为由 Kconfig 的 LV_PINYIN_IME_VOICE_TAP_MODE 决定：
 *   TOGGLE / BOTH —— 没在识别就开始，正在识别就结束
 *   PTT           —— 没在识别就开始（等松手才结束，见 voice_key_released）
 */
void lv_pinyin_ime_voice_key_pressed(lv_obj_t *ime);

/** 键盘上的语音键被松开（长按对讲模式用它结束识别）。 */
void lv_pinyin_ime_voice_key_released(lv_obj_t *ime);

/** 取消（长按语音键、或停止时想丢弃）。 */
void lv_pinyin_ime_voice_key_cancelled(lv_obj_t *ime);

/** 状态变化事件：应用可以据此做提示音、动画等。 */
lv_event_code_t lv_pinyin_ime_event_voice_state(void);

/** 识别出最终结果并已上屏时的事件。 */
lv_event_code_t lv_pinyin_ime_event_voice_result(void);

#ifdef __cplusplus
}
#endif
