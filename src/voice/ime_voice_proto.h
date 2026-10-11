/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 讯飞「语音听写（流式版）」WebAPI 的协议层。
 *
 * 这一层**不依赖 LVGL、不依赖 ESP-IDF、不依赖任何网络栈**：给它密钥、时间和
 * 一段 PCM，它负责拼鉴权 URL、拼 JSON 帧、解析返回的 JSON。所有网络收发都由
 * 调用方（host）完成 —— 见 include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h。
 *
 * 这样切分的目的是可测：主机端测试可以直接喂文档里的样例（包括动态修正
 * pgs/rpl、各种错误码、nbest/vinfo 变体）来断言解析结果，不需要联网。
 *
 * 协议依据：https://www.xfyun.cn/doc/asr/voicedictation/API.html
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 音频格式 */

/** 采样率：文档只允许 16k / 8k。 */
typedef enum {
    IME_VOICE_RATE_16K = 16000,
    IME_VOICE_RATE_8K = 8000,
} ime_voice_rate_t;

/**
 * 一帧音频的字节数。
 *
 * 文档的建议是「未压缩 PCM 每 40 ms 发 1280 B」（16k × 2 B × 0.04 s = 1280），
 * 并且要求每次发送的字节数是一帧的整数倍。8k 时同样的 40 ms 是 640 B。
 */
#define IME_VOICE_FRAME_MS 40
#define IME_VOICE_FRAME_BYTES_16K 1280
#define IME_VOICE_FRAME_BYTES_8K 640

/** 采样率对应的每帧字节数。 */
size_t ime_voice_frame_bytes(ime_voice_rate_t rate);

/** 采样率对应的 data.format 字符串（"audio/L16;rate=16000"）。 */
const char *ime_voice_format_str(ime_voice_rate_t rate);

/* --------------------------------------------------------------- 会话参数 */

#ifndef IME_VOICE_APPID_MAX
#define IME_VOICE_APPID_MAX 40
#endif
#ifndef IME_VOICE_KEY_MAX
#define IME_VOICE_KEY_MAX 68    /* 平台给的是 32 位，留足余量 */
#endif
#ifndef IME_VOICE_SECRET_MAX
#define IME_VOICE_SECRET_MAX 68
#endif
#ifndef IME_VOICE_ENDPOINT_MAX
#define IME_VOICE_ENDPOINT_MAX 128
#endif

/** 一次听写会话的业务参数（对应文档 common + business）。 */
typedef struct {
    char appid[IME_VOICE_APPID_MAX];
    char apikey[IME_VOICE_KEY_MAX];
    char apisecret[IME_VOICE_SECRET_MAX];
    char endpoint[IME_VOICE_ENDPOINT_MAX]; /* 含 wss:// 与路径，不含 query */
    char language[16];                     /* zh_cn / en_us / ... */
    char domain[32];                       /* iat / xfime-mianqie / medical ... */
    char accent[16];                       /* mandarin / ... */
    ime_voice_rate_t rate;
    bool dwa;        /* 动态修正 wpgs（仅中文普通话），与 vinfo 互斥 */
    int  eos_ms;     /* 后端点检测静默时长，默认 2000 */
    bool ptt;        /* 是否加标点，默认开 */
} ime_voice_params_t;

/**
 * 用默认值填满参数（端点、语种、领域、采样率、dwa、eos、标点）。
 *
 * 密钥字段留空，由调用方填。Kconfig 的默认值在 lv_pinyin_ime_voice.h 的
 * LV_PINYIN_IME_VOICE_CFG_DEFAULT() 里叠加。
 */
void ime_voice_params_defaults(ime_voice_params_t *p);

/** 参数是否足够发起一次会话（端点、appid、apikey、apisecret 都不能为空）。 */
bool ime_voice_params_valid(const ime_voice_params_t *p);

/* ----------------------------------------------------------------- 鉴权 URL */

/** 时钟偏移容差：文档说服务端允许 ±300 s，超出直接 403。 */
#define IME_VOICE_DATE_TOLERANCE_S 300

/**
 * 把 UTC 秒数按 RFC1123 写成 "Wed, 10 Jul 2019 07:35:43 GMT"。
 *
 * @param utc_seconds  Unix 时间戳（秒）
 * @param out          输出缓冲，至少 30 字节
 * @param out_size     缓冲大小
 * @return 成功与否；时间早于 1970-01-01 之外无法表示时返回 false
 */
bool ime_voice_rfc1123(int64_t utc_seconds, char *out, size_t out_size);

/**
 * 拼出带鉴权参数的完整 wss URL。
 *
 * 复刻文档 §接口鉴权：
 *   signature_origin = "host: $host\ndate: $date\nGET $path HTTP/1.1"
 *   signature        = base64(hmac_sha256(signature_origin, apiSecret))
 *   authorization    = base64('api_key="...", algorithm="hmac-sha256", '
 *                             'headers="host date request-line", signature="..."')
 *   url              = endpoint + "?authorization=<enc>&date=<enc>&host=<enc>"
 *
 * @param p        会话参数（用 endpoint / apikey / apisecret）
 * @param date     RFC1123 时间串
 * @param out      输出缓冲
 * @param out_size 缓冲大小；建议 >= 512
 * @return 成功与否（缓冲不够、端点格式不对都会失败）
 */
bool ime_voice_build_auth_url(const ime_voice_params_t *p, const char *date, char *out,
                              size_t out_size);

/** 同上，但时间取当前系统时间。需调用方先 SNTP 对时。 */
bool ime_voice_build_auth_url_now(const ime_voice_params_t *p, int64_t now_utc_seconds,
                                  char *out, size_t out_size);

/* ------------------------------------------------------------------- 请求帧 */

/** data.status 的三种取值。 */
typedef enum {
    IME_VOICE_STATUS_FIRST = 0, /**< 第一帧音频（带 common + business） */
    IME_VOICE_STATUS_MIDDLE = 1,/**< 中间音频（只带 data） */
    IME_VOICE_STATUS_LAST = 2,  /**< 最后一帧（只带 data.status） */
} ime_voice_status_t;

/** 首帧 JSON 的长度上界（不含音频 base64）。 */
#define IME_VOICE_FIRST_FRAME_OVERHEAD 512

/**
 * 音频帧 base64 之后的长度（含结尾 NUL）。
 *
 * 每 3 字节变 4 字符，再加向上取整与 NUL。
 */
size_t ime_voice_b64_len(size_t raw_len);

/**
 * 拼第一帧：common(common.app_id) + business + data(status=0)。
 *
 * @param p        会话参数
 * @param pcm      PCM 数据（可为 NULL，表示这一帧没有音频）
 * @param pcm_len  PCM 字节数
 * @param out      输出缓冲
 * @param out_size 缓冲大小，需 >= IME_VOICE_FIRST_FRAME_OVERHEAD + b64_len(pcm_len)
 * @return 写出的字节数，失败返回 0
 */
size_t ime_voice_build_first_frame(const ime_voice_params_t *p, const uint8_t *pcm,
                                   size_t pcm_len, char *out, size_t out_size);

/**
 * 拼中间帧：只带 data(status=1)。
 *
 * @return 写出的字节数，失败返回 0
 */
size_t ime_voice_build_audio_frame(const ime_voice_params_t *p, const uint8_t *pcm,
                                   size_t pcm_len, char *out, size_t out_size);

/**
 * 拼结束帧：{"data":{"status":2}}。
 *
 * 文档要求「数据上传完毕，客户端需要上传一次数据结束标识」；漏了这一帧服务端
 * 会一直等到 10 s 读超时（错误码 10200）。
 *
 * @return 写出的字节数，失败返回 0
 */
size_t ime_voice_build_last_frame(char *out, size_t out_size);

/* ------------------------------------------------------------------- 返回帧 */

#ifndef IME_VOICE_MAX_WORDS
#define IME_VOICE_MAX_WORDS 32      /* 一帧里的分词数上限 */
#endif
#ifndef IME_VOICE_WORD_MAX
#define IME_VOICE_WORD_MAX 32       /* 单个词/字的 UTF-8 字节上限 */
#endif
#ifndef IME_VOICE_MESSAGE_MAX
#define IME_VOICE_MESSAGE_MAX 96
#endif
#ifndef IME_VOICE_SID_MAX
#define IME_VOICE_SID_MAX 48
#endif
#ifndef IME_VOICE_MAX_NBEST
#define IME_VOICE_MAX_NBEST 5       /* 文档：nbest / wbest 取值 [1,5] */
#endif

/** 一帧里的一段话（data.result.ws[]）。 */
typedef struct {
    int  bg;                                  /**< 起始端点帧偏移（1 帧 = 10 ms） */
    char text[IME_VOICE_WORD_MAX * 2];        /**< 把本段 cw[] 连起来的结果 */
    int  nbest_count;                         /**< 多候选个数（未开就是 1） */
    char nbest[IME_VOICE_MAX_NBEST][IME_VOICE_WORD_MAX * 2];
} ime_voice_seg_t;

/** 解析后的返回帧。 */
typedef struct {
    int  code;
    char message[IME_VOICE_MESSAGE_MAX];
    char sid[IME_VOICE_SID_MAX];

    bool has_data;
    bool has_result;
    int  status;              /**< data.status：0 第一块 / 1 中间 / 2 最后一块 */
    int  sn;                  /**< data.result.sn，动态修正要靠它定位 */
    bool ls;                  /**< data.result.ls，最后一片 */
    bool has_sid;

    bool has_pgs;
    char pgs[8];              /**< "apd" 追加 / "rpl" 替换 */
    bool has_rg;
    int  rg_first;
    int  rg_last;

    bool has_vad;
    int  vad_bg;
    int  vad_ed;

    size_t seg_count;
    ime_voice_seg_t segs[IME_VOICE_MAX_WORDS];
} ime_voice_reply_t;

/** 解析一帧返回 JSON。@return 能不能解析（不代表 code == 0）。 */
bool ime_voice_parse_reply(const char *json, size_t len, ime_voice_reply_t *out);

/* --------------------------------------------------- 流式结果拼接（动态修正） */

#ifndef IME_VOICE_MAX_PARTS
#define IME_VOICE_MAX_PARTS 40
#endif
#ifndef IME_VOICE_PART_MAX
#define IME_VOICE_PART_MAX 96       /* 每段文本的 UTF-8 字节上限 */
#endif
#ifndef IME_VOICE_TRANSCRIPT_MAX
#define IME_VOICE_TRANSCRIPT_MAX 640
#endif

/**
 * 动态修正的段落表。
 *
 * 开了 dwa=wpgs 之后，每帧返回的 data.result.sn 是段号，pgs 决定这一段是
 * "apd"（追加到末尾）还是 "rpl"（替换 rg=[first,last] 这些段）。不开 wpgs 时
 * 所有段号都是 1，等价于"每次重写第 1 段"。
 *
 * 把段落表留在这里而不是直接拼字符串，是因为 rpl 会**回头改**已经显示的文本；
 * 用段落表重放一遍就能得到当前正确的整句，UI 那边只要显示这个结果。
 */
typedef struct {
    char parts[IME_VOICE_MAX_PARTS][IME_VOICE_PART_MAX];
    int  count;     /**< 已使用的段数（段号 1..count） */
} ime_voice_transcript_t;

/** 清空段落表。 */
void ime_voice_transcript_reset(ime_voice_transcript_t *t);

/**
 * 把一帧结果并进段落表。
 *
 * @param t     段落表
 * @param r     解析后的返回帧
 * @return 整句文本有没有变化（UI 据此决定要不要重画）
 */
bool ime_voice_transcript_apply(ime_voice_transcript_t *t, const ime_voice_reply_t *r);

/**
 * 取出当前整句（各段按顺序拼接）。没有内容时 out 写成空串。
 *
 * @return 当前长度（字节）
 */
size_t ime_voice_transcript_text(const ime_voice_transcript_t *t, char *out, size_t out_size);

/* -------------------------------------------------------------- 分词 / 文本 */

/**
 * 字形判定回调：字体能不能画出这个码点。
 *
 * 由调用方提供 —— UI 层用 lv_font_get_glyph_dsc() 实现，主机端测试用
 * ime_voice_cp_is_basic()。这样协议层不必知道字体从哪来。
 */
typedef bool (*ime_voice_cp_allowed_fn)(uint32_t cp);

/**
 * 把一帧里所有 ws[].cw[] 的首候选按顺序拼成整句（不含修正语义）。
 *
 * 用于 error / 单帧场景，也是 transcript_apply 取段文本的那一步。
 */
size_t ime_voice_reply_flat_text(const ime_voice_reply_t *r, char *out, size_t out_size);

/**
 * 把 @p in 里**渲染不出来**的字符删掉，剩下的写进 @p out。
 *
 * 为什么需要：识别的结果来自云端，字表里没有的字会画成缺字方框。所以文本进输入框
 * 之前要过一道筛子。
 *
 * ★ 判据不是"字符在不在 GB2312 里"，而是**当前字体到底有没有这个字形** —— 通过
 *   @p allowed 回调问出来。UI 层传一个基于 lv_font_get_glyph_dsc() 的实现
 *   （候选栏的翻页箭头已经在用同样的办法探测字形，见 lv_pinyin_ime_cand.c 的
 *   pager_caption()）。这样做有三个好处：
 *     1. 不用维护第二份字表：字表和字体一旦不同步就会出现"放行了却没画"的方框；
 *     2. 应用换更全的字体（lv_pinyin_ime_set_fonts()）时自动放宽，不需要改代码；
 *     3. 这一层仍然不依赖 LVGL —— 主机端测试传自己的回调即可。
 *
 * 注意：回调返回的是"能不能画"，不是"合不合法"。它返回 false 时字符只是被丢掉，
 * 不算错误。
 *
 * @param in          输入的 UTF-8 串
 * @param out         输出缓冲
 * @param out_size    输出缓冲大小
 * @param allowed     判定回调；为 NULL 时退回 ime_voice_cp_is_basic()（ASCII + CJK 区）
 * @param dropped     被丢弃的字符数，可为 NULL
 * @return 写出的字节数
 */
size_t ime_voice_filter_charset(const char *in, char *out, size_t out_size,
                                ime_voice_cp_allowed_fn allowed, size_t *dropped);

/**
 * 兜底的字形判定：ASCII 可打印区 + CJK 统一表意文字 + 常用全角标点。
 *
 * 给不方便接字体的情况用（主机端测试、非 LVGL 环境）。**判据偏宽**：它认为
 * U+4E00..U+9FA5 都有字形，而组件自带的 GB2312 子集字体里有一部分没有（GB2312
 * 之外的 9 716 字，见 docs/integration.md §5.3）—— 用真字体判定才是准的。
 */
bool ime_voice_cp_is_basic(uint32_t cp);

/**
 * 取一个 UTF-8 串里第一个字符的码点；空串或非法编码返回 0。
 *
 * 调用方要按字符判断字形时用它（LVGL 的解码器是 private 的，不能直接用）。
 */
uint32_t ime_voice_utf8_first_cp(const char *s);

/* ------------------------------------------------------------- 小小工具函数 */

/** base64 编码；返回写出的字符数（不含 NUL），缓冲不够返回 0。 */
size_t ime_voice_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size);

/** base64 解码；返回写出的字节数，失败返回 0。 */
size_t ime_voice_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size);

/** HMAC-SHA256；out 需要 32 字节。 */
void ime_voice_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                           size_t msg_len, uint8_t out[32]);

/** RFC3986 的 unreserved 之外全部百分号转义（讯飞要求对 query 值编码）。 */
size_t ime_voice_url_encode(const char *in, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
