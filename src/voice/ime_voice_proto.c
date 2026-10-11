/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 讯飞语音听写（流式版）的协议实现：鉴权 URL、请求帧、返回帧解析、动态修正拼接。
 *
 * 这一层没有任何外部依赖（不用 cJSON，不用 mbedtls），所以能整个搬到主机端跑测试
 * —— 见 test_apps/host_voice_test。设计取舍：
 *
 *   * **不引 cJSON**：组件的依赖表里只有 lvgl 和 libgooglepinyin，加一个 JSON 库
 *     会让所有接入方都被迫多带一份。而这里要解析的结构是**固定**的（只有
 *     code/message/sid/data.result.{sn,ls,pgs,rg,ws[].cw[].w}），手写一个够用的
 *     解析器比背一个库更可控，也不会因为上游改 API 而崩。
 *
 *   * **解析前先做一次 \uXXXX 就地解码**：服务端会用 \uXXXX 转义汉字。就地解码
 *     不会让字符串变长（\uXXXX 是 6 字节，解出来最多 3 字节 UTF-8），所以可以在
 *     一块缓冲里原地做，之后所有字符串都可以按普通 UTF-8 处理。
 *
 *   * **段落表**：开了动态修正（dwa=wpgs）后服务端会回头替换已发的段落，所以
 *     不能边收边拼字符串，得先存段落再重放（ime_voice_transcript_*）。
 */

#include "voice/ime_voice_proto.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ 音频格式 */

size_t ime_voice_frame_bytes(ime_voice_rate_t rate)
{
    return (rate == IME_VOICE_RATE_8K) ? IME_VOICE_FRAME_BYTES_8K
                                       : IME_VOICE_FRAME_BYTES_16K;
}

const char *ime_voice_format_str(ime_voice_rate_t rate)
{
    return (rate == IME_VOICE_RATE_8K) ? "audio/L16;rate=8000" : "audio/L16;rate=16000";
}

/* --------------------------------------------------------------- 会话参数 */

void ime_voice_params_defaults(ime_voice_params_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    snprintf(p->endpoint, sizeof(p->endpoint), "%s", "wss://iat-api.xfyun.cn/v2/iat");
    snprintf(p->language, sizeof(p->language), "%s", "zh_cn");
    snprintf(p->domain, sizeof(p->domain), "%s", "iat");
    snprintf(p->accent, sizeof(p->accent), "%s", "mandarin");
    p->rate = IME_VOICE_RATE_16K;
    p->dwa = true;
    p->eos_ms = 2000;
    p->ptt = true;
}

bool ime_voice_params_valid(const ime_voice_params_t *p)
{
    if (p == NULL) {
        return false;
    }
    if (p->endpoint[0] == '\0' || strstr(p->endpoint, "://") == NULL) {
        return false;
    }
    if (p->appid[0] == '\0' || p->apikey[0] == '\0' || p->apisecret[0] == '\0') {
        return false;
    }
    return true;
}

/* ----------------------------------------------------------------- 鉴权 URL */

bool ime_voice_rfc1123(int64_t utc_seconds, char *out, size_t out_size)
{
    static const char *const wday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (out == NULL || out_size < 30u) {
        return false;
    }
    /* 1970-01-01 是星期四。负数时间（时钟没对）直接拒绝。 */
    if (utc_seconds < 0) {
        return false;
    }

    const int64_t days = utc_seconds / 86400;
    int64_t secs = utc_seconds % 86400;
    const int hour = (int)(secs / 3600);
    secs %= 3600;
    const int min = (int)(secs / 60);
    const int sec = (int)(secs % 60);
    const int wd = (int)((days + 4) % 7);

    /* 民用历法：从 1970 起逐年减，再逐月减。范围足够（年份会被 printf 限制）。 */
    int64_t d = days;
    int year = 1970;
    while (true) {
        const bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        const int ylen = leap ? 366 : 365;
        if (d < ylen) {
            break;
        }
        d -= ylen;
        year++;
        if (year > 9999) {
            return false;               /* 时钟明显不对 */
        }
    }
    const bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    static const int mlen[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = 0;
    while (m < 12) {
        int len = mlen[m] + ((m == 1 && leap) ? 1 : 0);
        if (d < len) {
            break;
        }
        d -= len;
        m++;
    }

    const int n = snprintf(out, out_size, "%s, %02d %s %04d %02d:%02d:%02d GMT", wday[wd],
                           (int)d + 1, mon[m], year, hour, min, sec);
    return (n > 0 && (size_t)n < out_size);
}

bool ime_voice_build_auth_url(const ime_voice_params_t *p, const char *date,
                              ime_voice_auth_scratch_t *s, char *out, size_t out_size)
{
    if (p == NULL || date == NULL || s == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    /* 拆 endpoint：scheme://host/path */
    const char *scheme_end = strstr(p->endpoint, "://");
    if (scheme_end == NULL) {
        return false;
    }
    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    if (path == NULL || path[1] == '\0') {
        return false;
    }
    const size_t host_len = (size_t)(path - host);
    if (host_len == 0 || host_len >= sizeof(s->host)) {
        return false;
    }
    memcpy(s->host, host, host_len);
    s->host[host_len] = '\0';

    /* signature_origin = "host: $host\ndate: $date\nGET $path HTTP/1.1" */
    const int olen = snprintf(s->origin, sizeof(s->origin),
                              "host: %s\ndate: %s\nGET %s HTTP/1.1", s->host, date, path);
    if (olen <= 0 || (size_t)olen >= sizeof(s->origin)) {
        return false;
    }

    uint8_t mac[32];
    ime_voice_hmac_sha256((const uint8_t *)p->apisecret, strlen(p->apisecret),
                          (const uint8_t *)s->origin, (size_t)olen, mac);

    if (ime_voice_b64_encode(mac, sizeof(mac), s->signature, sizeof(s->signature)) == 0) {
        return false;
    }

    /* authorization_origin：注意格式里的空格与引号是协议的一部分，一个都不能少
     * （文档 §authorization参数生成规则 第 6 步）。 */
    const int alen = snprintf(s->auth_origin, sizeof(s->auth_origin),
                              "api_key=\"%s\", algorithm=\"hmac-sha256\", "
                              "headers=\"host date request-line\", signature=\"%s\"",
                              p->apikey, s->signature);
    if (alen <= 0 || (size_t)alen >= sizeof(s->auth_origin)) {
        return false;
    }

    if (ime_voice_b64_encode((const uint8_t *)s->auth_origin, (size_t)alen, s->authorization,
                             sizeof(s->authorization)) == 0) {
        return false;
    }

    /* date 与 authorization 都要 URL 编码后再拼（文档示例里 date 的逗号与空格就是
     * %2C / %20）。 */
    if (ime_voice_url_encode(s->authorization, s->enc_auth, sizeof(s->enc_auth)) == 0 ||
        ime_voice_url_encode(date, s->enc_date, sizeof(s->enc_date)) == 0 ||
        ime_voice_url_encode(s->host, s->enc_host, sizeof(s->enc_host)) == 0) {
        return false;
    }

    const int n = snprintf(out, out_size, "%s?authorization=%s&date=%s&host=%s", p->endpoint,
                           s->enc_auth, s->enc_date, s->enc_host);
    return (n > 0 && (size_t)n < out_size);
}

bool ime_voice_build_auth_url_now(const ime_voice_params_t *p, int64_t now_utc_seconds,
                                  ime_voice_auth_scratch_t *scratch, char *out,
                                  size_t out_size)
{
    char date[40];
    if (!ime_voice_rfc1123(now_utc_seconds, date, sizeof(date))) {
        return false;
    }
    return ime_voice_build_auth_url(p, date, scratch, out, out_size);
}

/* ------------------------------------------------------------------- 请求帧 */

size_t ime_voice_b64_len(size_t raw_len)
{
    return ((raw_len + 2u) / 3u) * 4u + 1u;
}

size_t ime_voice_build_first_frame(const ime_voice_params_t *p, const uint8_t *pcm,
                                   size_t pcm_len, char *out, size_t out_size)
{
    if (p == NULL || out == NULL) {
        return 0;
    }
    const size_t b64_len = ime_voice_b64_len(pcm_len);
    if (out_size < IME_VOICE_FIRST_FRAME_OVERHEAD + b64_len) {
        return 0;
    }

    /* business 里的可选参数按文档默认值省略：dwa 只在开了才发（文档只定义了
     * "wpgs" 一个取值），ptt 的默认值就是 1，只有关掉时才需要显式发 0。 */
    const char *dwa_part = p->dwa ? "\"dwa\":\"wpgs\"," : "";
    const char *ptt_part = p->ptt ? "" : "\"ptt\":0,";

    int n = snprintf(out, out_size,
                     "{\"common\":{\"app_id\":\"%s\"},"
                     "\"business\":{\"language\":\"%s\",\"domain\":\"%s\","
                     "\"accent\":\"%s\",\"eos\":%d,%s%s\"vad_eos\":%d},"
                     "\"data\":{\"status\":%d,\"format\":\"%s\",\"encoding\":\"raw\","
                     "\"audio\":\"",
                     p->appid, p->language, p->domain, p->accent, p->eos_ms, dwa_part,
                     ptt_part, p->eos_ms, (int)IME_VOICE_STATUS_FIRST,
                     ime_voice_format_str(p->rate));
    if (n <= 0 || (size_t)n >= out_size) {
        return 0;
    }

    const size_t b64 = ime_voice_b64_encode(pcm, pcm_len, out + n, out_size - (size_t)n);
    if (b64 == 0 && pcm_len > 0) {
        return 0;
    }
    n += (int)b64;

    const int tail = snprintf(out + n, out_size - (size_t)n, "\"}}");
    if (tail <= 0 || (size_t)tail >= out_size - (size_t)n) {
        return 0;
    }
    return (size_t)(n + tail);
}

size_t ime_voice_build_audio_frame(const ime_voice_params_t *p, const uint8_t *pcm,
                                   size_t pcm_len, char *out, size_t out_size)
{
    if (p == NULL || out == NULL) {
        return 0;
    }
    const size_t b64_len = ime_voice_b64_len(pcm_len);
    if (out_size < 160u + b64_len) {
        return 0;
    }

    int n = snprintf(out, out_size,
                     "{\"data\":{\"status\":%d,\"format\":\"%s\",\"encoding\":\"raw\","
                     "\"audio\":\"",
                     (int)IME_VOICE_STATUS_MIDDLE, ime_voice_format_str(p->rate));
    if (n <= 0 || (size_t)n >= out_size) {
        return 0;
    }

    const size_t b64 = ime_voice_b64_encode(pcm, pcm_len, out + n, out_size - (size_t)n);
    if (b64 == 0 && pcm_len > 0) {
        return 0;
    }
    n += (int)b64;

    const int tail = snprintf(out + n, out_size - (size_t)n, "\"}}");
    if (tail <= 0 || (size_t)tail >= out_size - (size_t)n) {
        return 0;
    }
    return (size_t)(n + tail);
}

size_t ime_voice_build_last_frame(char *out, size_t out_size)
{
    if (out == NULL || out_size < 20u) {
        return 0;
    }
    const int n = snprintf(out, out_size, "{\"data\":{\"status\":%d}}",
                           (int)IME_VOICE_STATUS_LAST);
    return (n > 0 && (size_t)n < out_size) ? (size_t)n : 0;
}

/* --------------------------------------------------------------- JSON 小工具 */

/*
 * ★ 为什么要有这个游标，而不是简单粗暴地 strstr("\"bg\"")：
 *   讯飞的返回帧里同一个键会在不同层级重复出现 —— data.result.ws[].bg 是"这段话
 *   从第几帧开始"，而 data.result.vad.ws[].bg 是"语音端点从第几帧开始"。用
 *   strstr 找 "bg" 会先命中哪个完全取决于服务端的字段顺序，一旦命中 vad 里的那个，
 *   每段话的偏移就全错了，而且**不会报错**（只是数值不对），极难发现。
 *
 *   所以这里按 JSON 结构走：cur_member() 只认**当前对象这一层**的键，遇到嵌套的
 *   '{' / '[' 整段跳过。多候选（nbest/wbest）在 cw[] 里也是靠它逐项取出来的。
 */
typedef struct {
    const char *p;      /* 下一个待解析的位置 */
    const char *end;    /* 缓冲区末尾（不含） */
} json_cur_t;

typedef struct {
    const char *key;
    size_t      key_len;
    const char *val;    /* 值的第一个字节 */
    const char *val_end;/* 值的最后一个字节 + 1 */
} json_member_t;

static json_cur_t json_cur(const char *p, const char *end)
{
    json_cur_t c;
    c.p = p;
    c.end = end;
    return c;
}

/**
 * ★ 为什么所有解析都要先调这个：
 *
 *   json_next() 靠括号匹配（json_match）跳过嵌套的对象/数组，而括号匹配是**扫到
 *   NUL 为止**的。调用方给的 buffer 只保证"长度 len 之内是 JSON"，len 之后是什么
 *   完全未知 —— 可能是另一个全局变量的内容，里面恰好有个 ']'，也可能一片乱码。
 *   直接扫过去，轻则解析失败，重则读到别人的内存。
 *
 *   所以先把 buffer 当 C 字符串量一次长度，用**字符串长度**当解析边界：
 *     - 量长度只看 len 字节，绝不越界；
 *     - 解析只在 [buf, buf+strlen) 内活动，绝不越界。
 *
 *   代价是 JSON 里若含有裸的 '\0' 会被截断。而 JSON 规范要求字符串里的 U+0000
 *   写成 \u0000（6 个可打印字符），所以真实报文里不会有裸 NUL —— 讯飞的返回帧
 *   确实如此。
 */
static size_t json_bounded_len(const char *buf, size_t len)
{
    size_t n = 0;
    while (n < len && buf[n] != '\0') {
        n++;
    }
    return n;
}

static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/* 找匹配的收尾括号；定义在下面（json_next 需要整段跳过嵌套的对象/数组）。 */
static const char *json_match(const char *p, char open, const char *end);

/**
 * 返回下一个顶层成员。
 *
 * @param c  游标；调用后指向本成员之后
 * @param m  成员信息；值为对象/数组时 val_end 指向与之匹配的收尾括号之后
 * @return 还有成员返回 true
 */
static bool json_next(json_cur_t *c, json_member_t *m)
{
    /* 跳过分隔符与成员的起始 '{' —— 对象与数组元素都靠这个进入内容。 */
    const char *p = json_skip_ws(c->p, c->end);
    while (p < c->end && (*p == ',' || *p == '{' || *p == '[')) {
        p = json_skip_ws(p + 1, c->end);
    }
    if (p >= c->end || *p == '}' || *p == ']' || *p != '"') {
        c->p = c->end;
        return false;
    }

    /* 键："..."（键里不会出现转义，但一并处理） */
    const char *key = p + 1;
    size_t key_len = 0;
    while (key[key_len] != '\0' && key + key_len < c->end && key[key_len] != '"') {
        key_len++;
    }
    if (key + key_len >= c->end) {
        c->p = c->end;
        return false;
    }

    const char *q = json_skip_ws(key + key_len + 1, c->end);
    if (q >= c->end || *q != ':') {
        c->p = c->end;
        return false;
    }
    q = json_skip_ws(q + 1, c->end);

    /* 值：对象/数组整段跳过，标量走到分隔符为止。 */
    const char *vend = q;
    if (q < c->end && (*q == '{' || *q == '[')) {
        const char *close = json_match(q, *q, c->end);
        if (close == NULL) {
            c->p = c->end;
            return false;
        }
        vend = close + 1;
    } else if (q < c->end && *q == '"') {
        const char *s = q + 1;
        while (s < c->end && *s != '"') {
            if (*s == '\\' && s + 1 < c->end) {
                s++;
            }
            s++;
        }
        vend = (s < c->end) ? s + 1 : c->end;
    } else {
        while (vend < c->end && *vend != ',' && *vend != '}' && *vend != ']') {
            vend++;
        }
        while (vend > q && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\n' ||
                            vend[-1] == '\r')) {
            vend--;
        }
    }

    m->key = key;
    m->key_len = key_len;
    m->val = q;
    m->val_end = vend;
    c->p = vend;
    return true;
}

static bool json_key_is(const json_member_t *m, const char *key)
{
    return m->key_len == strlen(key) && strncmp(m->key, key, m->key_len) == 0;
}

/** 值是不是对象 / 数组。 */
static bool json_is_container(const json_member_t *m, char want)
{
    return m->val < m->val_end && *m->val == want;
}

/**
 * 对象 / 数组的**内部**范围：跳过起始括号、去掉收尾括号。
 *
 * ★ 返回的是内容起点（跳过 '{' / '['），不是括号本身的位置 —— 游标的约定是
 *   "从内容开始遍历"，起点若是括号，json_next 会把里面的第一个键名当成数组元素。
 */
static void json_inner(const json_member_t *m, const char **from, const char **to)
{
    *from = m->val + 1;
    *to = (m->val_end > m->val + 1) ? m->val_end - 1 : m->val + 1;
}

/**
 * 取整数值。数字原样解析；字符串（服务端偶尔把数字写成字符串，文档也提到 sc 字段
 * 要兼容 float 与 int）则从引号里解析，兼容 "12" / "12.5"。
 */
static bool json_int_val(const json_member_t *m, int *out)
{
    const char *p = m->val;
    const char *end = m->val_end;

    if (p < end && *p == '"') {
        p++;
        if (end > p && end[-1] == '"') {
            end--;
        }
    }

    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 100000000L) {
            v = 100000000L;             /* 这些字段不会这么大，防溢出 */
        }
        p++;
    }
    if (p < end && *p == '.') {
        p++;                            /* 小数部分直接丢（调用方只要整数） */
        while (p < end && *p >= '0' && *p <= '9') {
            p++;
        }
    }
    *out = (int)(neg ? -v : v);
    return true;
}

static bool json_bool_val(const json_member_t *m, bool *out)
{
    const char *p = m->val;
    const char *end = m->val_end;

    /* 字符串形式的布尔值（"true" / "false"）也要认：服务端偶尔这样写。 */
    if (p < end && *p == '"') {
        p++;
        if (end > p && end[-1] == '"') {
            end--;
        }
    }

    if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }

    json_member_t plain = *m;
    plain.val = p;
    plain.val_end = end;
    int iv = 0;
    if (json_int_val(&plain, &iv)) {    /* 服务端有时用 0/1 */
        *out = (iv != 0);
        return true;
    }
    return false;
}

/** 取字符串值（处理 " \ / \n \t \r \b \f 这些转义；\u 已在前面就地解掉）。 */
static bool json_str_val(const json_member_t *m, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (m->val >= m->val_end || *m->val != '"') {
        return false;
    }

    size_t o = 0;
    for (const char *p = m->val + 1; p < m->val_end && *p != '"'; p++) {
        char ch = *p;
        if (ch == '\\' && p + 1 < m->val_end) {
            p++;
            switch (*p) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            default: ch = *p; break;
            }
        }
        if (o + 1u >= out_size) {
            break;                      /* 截断而不是失败：宁可少几个字也别整帧丢掉 */
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    return true;
}

/**
 * 找到想匹配的括号的结束位置。
 *
 * @param open   '{' 或 '['
 * @param end    解析边界（不含）。**必须传**，否则会扫出缓冲区外 —— 见
 *               json_bounded_len() 的说明。
 */
static const char *json_match(const char *p, char open, const char *end)
{
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    for (; p < end && *p != '\0'; p++) {
        if (in_string) {
            if (*p == '\\' && p + 1 < end) {
                p++;
            } else if (*p == '"') {
                in_string = false;
            }
            continue;
        }
        if (*p == '"') {
            in_string = true;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

static uint32_t hex4(const char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (uint32_t)(c - 'A' + 10);
        } else {
            return 0xFFFFFFFFu;
        }
    }
    return v;
}

static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/**
 * 就地把 JSON 里的 \uXXXX 转义解成 UTF-8 字节（含代理对）。
 *
 * 就地是安全的：\uXXXX 占 6 字节，解出来最多 4 字节（代理对是 12 → 4），结果永不
 * 比原文长。这样后面所有字符串都能当普通 UTF-8 处理，主机端测试也能直接喂文档
 * 里的样例（那些样例全是 \u 转义的）。
 */
static void json_unescape_unicode_inplace(char *s)
{
    char *w = s;
    for (char *p = s; *p != '\0';) {
        if (p[0] != '\\' || p[1] != 'u') {
            *w++ = *p++;
            continue;
        }
        const uint32_t cp = hex4(p + 2);
        if (cp == 0xFFFFFFFFu) {
            *w++ = *p++;                /* 坏转义：原样留着 */
            continue;
        }
        p += 6;

        uint32_t full = cp;
        /* 代理对：高位 D800-DBFF + 低位 DC00-DFFF 合成一个码点。 */
        if (cp >= 0xD800u && cp <= 0xDBFFu && p[0] == '\\' && p[1] == 'u') {
            const uint32_t lo = hex4(p + 2);
            if (lo != 0xFFFFFFFFu && lo >= 0xDC00u && lo <= 0xDFFFu) {
                full = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                p += 6;
            }
        }
        w += utf8_encode(full, w);
    }
    *w = '\0';
}

/* --------------------------------------------------------------- 返回帧解析 */

/**
 * 取对象里的成员（键值对）。
 *
 * 键的顺序无关，但只在**当前这一层**找：遇到嵌套的对象/数组整段跳过。别拿它遍历
 * 数组 —— 数组的元素不是键值对，会一个都取不到（用 json_elem）。
 *
 * @param from 对象内容起点（即 '{' 之后）
 * @param to   对象内容终点（即 '}' 的位置）
 */
static bool json_find(const char *from, const char *to, const char *key, json_member_t *out)
{
    json_cur_t c = json_cur(from, to);
    json_member_t m;
    while (json_next(&c, &m)) {
        if (json_key_is(&m, key)) {
            *out = m;
            return true;
        }
    }
    return false;
}

/**
 * 取数组里的下一个元素。
 *
 * ★ 为什么必须和 json_next() 分开：json_next() 会跳过 '{' 去找**键名**，所以对
 *   `[{"bg":7,...}]` 它会直接把里面的 "bg" 当成成员返回 —— 元素这一层就丢了，
 *   于是 ws[] 永远解析不出内容（本函数的由来）。
 *
 * 游标 c 应指向数组内容起点（即 '[' 之后）。
 */
static bool json_elem(json_cur_t *c, json_member_t *out)
{
    const char *p = json_skip_ws(c->p, c->end);
    if (p >= c->end) {
        return false;
    }
    if (*p == ',') {
        p = json_skip_ws(p + 1, c->end);
    }
    if (p >= c->end || *p == ']' || *p == '}') {
        c->p = c->end;
        return false;
    }

    const char *vend = p;
    if (*p == '{' || *p == '[') {
        const char *close = json_match(p, *p, c->end);
        if (close == NULL) {
            c->p = c->end;
            return false;
        }
        vend = close + 1;
    } else if (*p == '"') {
        const char *s = p + 1;
        while (s < c->end && *s != '"') {
            if (*s == '\\' && s + 1 < c->end) {
                s++;
            }
            s++;
        }
        vend = (s < c->end) ? s + 1 : c->end;
    } else {
        while (vend < c->end && *vend != ',' && *vend != ']' && *vend != '}') {
            vend++;
        }
        while (vend > p && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\n' ||
                            vend[-1] == '\r')) {
            vend--;
        }
    }

    out->key = p;
    out->key_len = (size_t)(vend - p);
    out->val = p;
    out->val_end = vend;
    c->p = vend;
    return true;
}

/** 解析 data.result.ws[] 里的 cw[]（含 nbest/wbest 的多候选）。 */
static void parse_cw(const json_member_t *cw, ime_voice_seg_t *seg)
{
    const char *from = NULL;
    const char *to = NULL;
    json_inner(cw, &from, &to);

    json_cur_t c = json_cur(from, to);
    json_member_t item;
    int word = 0;
    while (json_elem(&c, &item)) {
        if (!json_is_container(&item, '{')) {
            continue;
        }
        const char *ef = NULL;
        const char *et = NULL;
        json_inner(&item, &ef, &et);
        json_member_t w;
        if (json_find(ef, et, "w", &w)) {
            if (word == 0) {
                (void)json_str_val(&w, seg->text, sizeof(seg->text));
            } else if (word < IME_VOICE_MAX_NBEST) {
                (void)json_str_val(&w, seg->nbest[word], sizeof(seg->nbest[0]));
            }
            word++;
        }
    }
    seg->nbest_count = (word > IME_VOICE_MAX_NBEST) ? IME_VOICE_MAX_NBEST : word;
}

/** 解析一个 ws[] 元素。 */
static void parse_seg(const json_member_t *item, ime_voice_seg_t *seg)
{
    memset(seg, 0, sizeof(*seg));

    const char *from = NULL;
    const char *to = NULL;
    json_inner(item, &from, &to);

    json_cur_t c = json_cur(from, to);
    json_member_t m;
    while (json_next(&c, &m)) {
        if (json_key_is(&m, "bg")) {
            (void)json_int_val(&m, &seg->bg);
        } else if (json_key_is(&m, "cw") && json_is_container(&m, '[')) {
            parse_cw(&m, seg);
        }
    }
}

/** 解析 data.result。 */
static void parse_result(const json_member_t *result, ime_voice_reply_t *out)
{
    const char *from = NULL;
    const char *to = NULL;
    json_inner(result, &from, &to);
    out->has_result = true;

    json_cur_t c = json_cur(from, to);
    json_member_t m;
    while (json_next(&c, &m)) {
        if (json_key_is(&m, "sn")) {
            (void)json_int_val(&m, &out->sn);
        } else if (json_key_is(&m, "ls")) {
            (void)json_bool_val(&m, &out->ls);
        } else if (json_key_is(&m, "pgs")) {
            out->has_pgs = json_str_val(&m, out->pgs, sizeof(out->pgs));
        } else if (json_key_is(&m, "rg") && json_is_container(&m, '[')) {
            const char *rf = NULL;
            const char *rt = NULL;
            json_inner(&m, &rf, &rt);
            json_cur_t rc = json_cur(rf, rt);
            json_member_t item;
            if (json_elem(&rc, &item) && json_int_val(&item, &out->rg_first) &&
                json_elem(&rc, &item) && json_int_val(&item, &out->rg_last)) {
                out->has_rg = true;
            }
        } else if (json_key_is(&m, "vad") && json_is_container(&m, '{')) {
            /* 只取第一个端点的 bg/ed：够用来做"检测到语音/静音"的提示。 */
            const char *vf = NULL;
            const char *vt = NULL;
            json_inner(&m, &vf, &vt);
            json_member_t ws;
            if (json_find(vf, vt, "ws", &ws) && json_is_container(&ws, '[')) {
                const char *af = NULL;
                const char *at = NULL;
                json_inner(&ws, &af, &at);
                json_cur_t vc = json_cur(af, at);
                json_member_t item;
                if (json_elem(&vc, &item) && json_is_container(&item, '{')) {
                    const char *ef = NULL;
                    const char *et = NULL;
                    json_inner(&item, &ef, &et);
                    json_member_t f;
                    if (json_find(ef, et, "bg", &f)) {
                        (void)json_int_val(&f, &out->vad_bg);
                    }
                    if (json_find(ef, et, "ed", &f)) {
                        (void)json_int_val(&f, &out->vad_ed);
                    }
                    out->has_vad = true;
                }
            }
        } else if (json_key_is(&m, "ws") && json_is_container(&m, '[')) {
            const char *wf = NULL;
            const char *wt = NULL;
            json_inner(&m, &wf, &wt);
            json_cur_t wc = json_cur(wf, wt);
            json_member_t item;
            while (out->seg_count < IME_VOICE_MAX_WORDS && json_elem(&wc, &item)) {
                if (!json_is_container(&item, '{')) {
                    continue;
                }
                parse_seg(&item, &out->segs[out->seg_count]);
                out->seg_count++;
            }
        }
    }
}

bool ime_voice_parse_reply(const char *json, size_t len, ime_voice_reply_t *out)
{
    if (json == NULL || out == NULL || len == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /*
     * 就地解码 + 就地补齐 NUL：网络回调给的长度未必包含结尾 NUL，而 \u 解码与括号
     * 匹配都按 C 字符串走。调用方要保证 buffer 后面还有 1 字节可写 —— 见
     * lv_pinyin_ime_voice.h 里 lv_pinyin_ime_voice_ws_message() 的说明。
     *
     * buf[len] 之后一律不看：解析边界用 json_bounded_len() 量出来的实际长度
     * （见该函数的说明）。
     */
    char *buf = (char *)json;
    buf[len] = '\0';
    json_unescape_unicode_inplace(buf);

    const size_t nlen = json_bounded_len(buf, len);

    json_cur_t c = json_cur(buf, buf + nlen);
    json_member_t m;
    bool have_code = false;
    while (json_next(&c, &m)) {
        if (json_key_is(&m, "code")) {
            if (!json_int_val(&m, &out->code)) {
                return false;
            }
            have_code = true;
        } else if (json_key_is(&m, "message")) {
            (void)json_str_val(&m, out->message, sizeof(out->message));
        } else if (json_key_is(&m, "sid")) {
            out->has_sid = json_str_val(&m, out->sid, sizeof(out->sid));
        } else if (json_key_is(&m, "data") && json_is_container(&m, '{')) {
            out->has_data = true;
            json_member_t d;
            const char *df = NULL;
            const char *dt = NULL;
            json_inner(&m, &df, &dt);
            if (json_find(df, dt, "status", &d)) {
                (void)json_int_val(&d, &out->status);
            }
            if (json_find(df, dt, "result", &d) && json_is_container(&d, '{')) {
                parse_result(&d, out);
            }
        }
    }

    return have_code;
}

size_t ime_voice_reply_flat_text(const ime_voice_reply_t *r, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (r == NULL) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < r->seg_count; i++) {
        const char *t = r->segs[i].text;
        for (; *t != '\0'; t++) {
            if (o + 1u >= out_size) {
                out[o] = '\0';
                return o;
            }
            out[o++] = *t;
        }
    }
    out[o] = '\0';
    return o;
}

/* --------------------------------------------------------- 动态修正段落表 */

void ime_voice_transcript_reset(ime_voice_transcript_t *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

bool ime_voice_transcript_apply(ime_voice_transcript_t *t, const ime_voice_reply_t *r,
                                char *before_scratch)
{
    if (t == NULL || r == NULL || before_scratch == NULL) {
        return false;
    }

    /*
     * 这一段属于哪个段号：
     *   - 开了动态修正时服务端给 sn（从 1 开始计数）；
     *   - 没开 wpgs 时没有 sn，等价于"每次都重写第 1 段"（文档：未开启动态修正
     *     时每次返回都是对之前结果的追加 —— 追加的是"整句"，所以直接覆盖第 1 段
     *     就得到最新整句）。
     */
    int sn = (r->sn >= 1) ? r->sn : 1;
    if (sn > IME_VOICE_MAX_PARTS) {
        sn = IME_VOICE_MAX_PARTS;
    }

    /* 把这一帧的所有 ws[].cw[0].w 拼起来，就是这一段的文本。 */
    char text[IME_VOICE_PART_MAX];
    ime_voice_reply_flat_text(r, text, sizeof(text));

    /*
     * ★ 先记住改动前的整句，再落盘，最后比较。
     *
     *   原来直接和 t->parts[] 比"要不要改"，但 rpl 分支一定会清空被替换的段落，
     *   于是"清空"这一步永远被算成"变了" —— 重复喂同一帧也会报变化，UI 每帧都
     *   白重画一次。
     *
     * ★ 快照由调用方给缓冲（before_scratch，至少 IME_VOICE_TRANSCRIPT_MAX 字节），
     *   不在这里开局部数组：这份快照 640 B，而调用方是 4 KB 栈的任务，能省则省。
     */
    ime_voice_transcript_text(t, before_scratch, IME_VOICE_TRANSCRIPT_MAX);
    const size_t before_len = strlen(before_scratch);

    /*
     * 按 pgs 的值分派。pgs 是服务端对"这一片是什么"的声明：
     *
     *   1. pgs == "apd"   追加：这是**新的一段**，写到段号 sn 上。
     *   2. pgs == "rpl"   替换：回头改写第 rg[0]..rg[1] 段（文档："替换前面的部分
     *                     结果，替换范围为 rg 字段"）。即使 rg 缺失/损坏也走替换
     *                     最后一段，绝不能当追加 —— 那会把被修正掉的旧文本留在屏上。
     *   3. 没有 pgs       见下面那段长注释。
     */
    const bool pgs_is_append = r->has_pgs && (strcmp(r->pgs, "apd") == 0);
    const bool is_final = (r->status == 2) || r->ls;

    if (pgs_is_append) {
        snprintf(t->parts[sn - 1], sizeof(t->parts[0]), "%s", text);
        if (t->count < sn) {
            t->count = sn;
        }
    } else if (r->has_pgs) {
        /* rpl（或任何非 apd 的取值）：替换。rg 缺失时退化成"替换最后一段"。 */
        int first;
        int last;
        if (r->has_rg) {
            first = r->rg_first;
            last = r->rg_last;
        } else {
            first = (t->count > 0) ? t->count : 1;
            last = first;
        }
        if (first < 1) {
            first = 1;
        }
        if (last < first) {
            last = first;
        }
        if (last > IME_VOICE_MAX_PARTS) {
            last = IME_VOICE_MAX_PARTS;
        }

        /* 被替换掉的几段清空，文本落到 first 上。 */
        for (int i = first; i <= last; i++) {
            t->parts[i - 1][0] = '\0';
        }
        snprintf(t->parts[first - 1], sizeof(t->parts[0]), "%s", text);
        if (t->count < last) {
            t->count = last;
        }
    } else if (is_final) {
        /*
         * ★ 末帧（status=2 / ls=true）且**没有 pgs**：忽略它的内容。
         *
         * 这是被测试逼出来的一条规则（test_apps/host_voice_test 的 test_streaming）。
         * 服务端的收尾帧不带 pgs，而且它**重发最后一段**：
         *
         *     sn=1 apd "今天"        → 今天
         *     sn=2 apd "天气"        → 今天天气
         *     sn=2 rpl [1,1] "明天"  → 明天天气
         *     status=2, ls=true, sn=2, 无 pgs, ws=[{w:"明天"}]   ← 收尾帧
         *
         *   早期版本把这帧按 sn 当段号写（sn=2 → 写第 2 段），第 2 段从"天气"变成
         *   "明天"，整句成了 **"明天明天"** —— 收尾帧重发的内容被当成了新的一段。
         *   真机表现：说完一句之后，屏上最后一小段文字重复一遍。
         *
         * 为什么"忽略"比"再写一次"安全：
         *   - 内容与最后一段相同（上面这种）→ 写不写都一样，忽略则永远不会写出重复；
         *   - 内容不同 → 说明它带的是新东西，但既然没有 pgs，服务端并没有声明它是
         *     新的一段；强行落盘就可能把整句搞乱。取"宁可少写也不要写错"。
         *
         * 注意：这只影响收尾帧。前面每一片都带 pgs（apd/rpl），文本在收尾之前
         * 就已经完整地显示出来了 —— 所以忽略收尾帧不会丢字，用户看到的仍是逐字
         * 实时刷新的效果。
         */
        (void)0;
    } else {
        /*
         * 既没有 pgs、也不是末帧：按段号写。
         *
         * 这是**没开 wpgs** 的情况：文档说"每次返回都是对之前结果的追加"，而追加的
         * 是**整句**。所以每一帧都覆盖第 1 段就是最新整句；服务端不给 sn 时 sn 取 1，
         * 正好落在这里。
         */
        snprintf(t->parts[sn - 1], sizeof(t->parts[0]), "%s", text);
        if (t->count < sn) {
            t->count = sn;
        }
    }

    /*
     * 改动后的整句长度直接算出来，不必再开一个缓冲存它。
     * 先用长度比（绝大多数帧在这一步就判完），长度相同才逐段比内容。
     */
    size_t after_len = 0;
    for (int i = 0; i < t->count && i < IME_VOICE_MAX_PARTS; i++) {
        after_len += strlen(t->parts[i]);
    }
    if (after_len != before_len) {
        return true;
    }

    size_t off = 0;
    for (int i = 0; i < t->count && i < IME_VOICE_MAX_PARTS; i++) {
        const size_t n = strlen(t->parts[i]);
        if (strncmp(before_scratch + off, t->parts[i], n) != 0) {
            return true;
        }
        off += n;
    }
    return false;
}

size_t ime_voice_transcript_text(const ime_voice_transcript_t *t, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (t == NULL) {
        return 0;
    }
    size_t o = 0;
    for (int i = 0; i < t->count && i < IME_VOICE_MAX_PARTS; i++) {
        const char *s = t->parts[i];
        for (; *s != '\0'; s++) {
            if (o + 1u >= out_size) {
                out[o] = '\0';
                return o;
            }
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    return o;
}
