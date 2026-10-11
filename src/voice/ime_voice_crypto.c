/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-256 / HMAC-SHA256 / base64 / URL 编码。
 *
 * ★ 为什么自带而不用 mbedtls：
 *   1. 目标板跑的是 ESP-IDF 6.1，它带的 **Mbed TLS 4.0（TF-PSA-Crypto 1.0）** 把
 *      `mbedtls/md.h` 挪进了 `tf-psa-crypto/`，并且明确公告「HMAC operations are
 *      no longer supported via MD」—— 老的 `mbedtls_md_setup(&ctx, info, 1)` 写法
 *      在 4.0 上会静默失效。自带一份就不用跟着上游 API 变动。
 *   2. 组件因此仍然**零外部依赖**：主机端测试不必链接 mbedtls，换 IDF 版本也不会坏。
 *   3. 这一层只做两件标准事（HMAC-SHA256、base64），有文档样例与 RFC 测试向量可
 *      逐字节校验 —— 见 test_apps/host_voice_test。
 *
 * 实现按 RFC 6234 / FIPS 180-4 写；base64 按 RFC 4648（带 '=' 补齐，解码宽容换行）。
 */

#include "ime_voice_proto.h"

#include <string.h>

/* ============================================================== SHA-256 */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   buf_len;
} sha256_ctx_t;

static const uint32_t s_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(sha256_ctx_t *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

    for (int i = 0; i < 64; i++) {
        const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + s1 + ch + s_sha256_k[i] + w[i];
        const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx_t *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au; c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bits = 0;
    c->buf_len = 0;
}

static void sha256_update(sha256_ctx_t *c, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    c->bits += (uint64_t)len * 8u;

    if (c->buf_len > 0) {
        const size_t need = 64u - c->buf_len;
        const size_t take = (len < need) ? len : need;
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += take;
        data += take;
        len -= take;
        if (c->buf_len == 64u) {
            sha256_block(c, c->buf);
            c->buf_len = 0;
        }
    }
    while (len >= 64u) {
        sha256_block(c, data);
        data += 64u;
        len -= 64u;
    }
    if (len > 0) {
        memcpy(c->buf, data, len);
        c->buf_len = len;
    }
}

static void sha256_final(sha256_ctx_t *c, uint8_t out[32])
{
    const uint64_t bits = c->bits;

    /* 0x80 之后补零到 56 mod 64，再放 8 字节大端长度。 */
    const uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    const uint8_t zero = 0x00;
    while (c->buf_len != 56u) {
        sha256_update(c, &zero, 1);
    }

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(c, len_be, sizeof(len_be));

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void ime_voice_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                           size_t msg_len, uint8_t out[32])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    sha256_ctx_t c;

    memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) {
        /* 比块长的密钥先哈希（RFC 2104）。平台给的密钥只有 32 字节，走不到这里，
         * 但留着它才是完整的 HMAC。 */
        sha256_init(&c);
        sha256_update(&c, key, key_len);
        sha256_final(&c, k);
    } else if (key != NULL && key_len > 0) {
        memcpy(k, key, key_len);
    }

    for (size_t i = 0; i < sizeof(k); i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }

    sha256_init(&c);
    sha256_update(&c, ipad, sizeof(ipad));
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, sizeof(opad));
    sha256_update(&c, inner, sizeof(inner));
    sha256_final(&c, out);
}

/* ================================================================ base64 */

static const char s_b64_alpha[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** 反查表：-1 表示不是合法 base64 字符（失败），-2 表示 '=' 填充。 */
static int8_t b64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (int8_t)(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return (int8_t)(ch - 'a' + 26);
    }
    if (ch >= '0' && ch <= '9') {
        return (int8_t)(ch - '0' + 52);
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    if (ch == '=') {
        return -2;
    }
    return -1;
}

size_t ime_voice_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    const size_t need = ((in_len + 2u) / 3u) * 4u;
    if (need + 1u > out_size) {
        return 0;
    }
    if (in == NULL && in_len > 0) {
        return 0;
    }

    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= in_len) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = s_b64_alpha[(v >> 18) & 0x3fu];
        out[o++] = s_b64_alpha[(v >> 12) & 0x3fu];
        out[o++] = s_b64_alpha[(v >> 6) & 0x3fu];
        out[o++] = s_b64_alpha[v & 0x3fu];
        i += 3u;
    }
    const size_t rest = in_len - i;
    if (rest == 1u) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = s_b64_alpha[(v >> 18) & 0x3fu];
        out[o++] = s_b64_alpha[(v >> 12) & 0x3fu];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rest == 2u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = s_b64_alpha[(v >> 18) & 0x3fu];
        out[o++] = s_b64_alpha[(v >> 12) & 0x3fu];
        out[o++] = s_b64_alpha[(v >> 6) & 0x3fu];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

size_t ime_voice_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
    if (in == NULL || out == NULL) {
        return 0;
    }

    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0;

    for (size_t i = 0; i < in_len; i++) {
        const char ch = in[i];
        /* 宽容处理换行/空白：服务端不会插，但调用方可能从文件里读样例。 */
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        const int8_t v = b64_value(ch);
        if (v == -2) {
            break;              /* '=' 之后不再有有效数据 */
        }
        if (v < 0) {
            return 0;
        }
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= out_size) {
                return 0;
            }
            out[o++] = (uint8_t)((acc >> nbits) & 0xffu);
        }
    }
    return o;
}

/* ============================================================ URL 编码 */

size_t ime_voice_url_encode(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    if (in == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        const unsigned char c = *p;
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            if (o + 1u >= out_size) {
                return 0;
            }
            out[o++] = (char)c;
        } else {
            if (o + 3u >= out_size) {
                return 0;
            }
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0fu];
        }
    }
    out[o] = '\0';
    return o;
}

/* ====================================================== 字形可用性过滤 */

uint32_t ime_voice_utf8_first_cp(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80u) {
        return p[0];
    }
    if ((p[0] & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
    }
    if ((p[0] & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
               (uint32_t)(p[2] & 0x3Fu);
    }
    if ((p[0] & 0xF8u) == 0xF0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u &&
        (p[3] & 0xC0u) == 0x80u) {
        return ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
               ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    }
    return 0;
}

bool ime_voice_cp_is_basic(uint32_t cp)
{
    if (cp >= 0x20u && cp <= 0x7Eu) {
        return true;                    /* ASCII 可打印 */
    }
    if (cp == '\n' || cp == '\t') {
        return true;                    /* 输入框可能是多行 */
    }
    if (cp >= 0x4E00u && cp <= 0x9FA5u) {
        return true;                    /* CJK 统一表意文字 */
    }
    /* 常用全角标点、箭头、货币、数学符号：组件的字表里都收了。 */
    switch (cp) {
    case 0x00A3: case 0x00A5: case 0x00A7: case 0x00A8: case 0x00B0: case 0x00B1:
    case 0x00D7: case 0x00F7: case 0x2018: case 0x2019: case 0x201C: case 0x201D:
    case 0x2013: case 0x2014: case 0x2026: case 0x2190: case 0x2191: case 0x2192:
    case 0x2193: case 0x21D2: case 0x21D4: case 0x2248: case 0x2260: case 0x2264:
    case 0x2265: case 0x2032: case 0x2033: case 0x20AC: case 0x03A9: case 0x03BC:
    case 0x3001: case 0x3002: case 0x300A: case 0x300B:
    case 0xFF01: case 0xFF08: case 0xFF09: case 0xFF0C: case 0xFF1A: case 0xFF1B:
    case 0xFF1F: case 0xFF5E: case 0xFFE0: case 0xFFE1: case 0xFFE3: case 0xFFE5:
        return true;
    default:
        return false;
    }
}

size_t ime_voice_filter_charset(const char *in, char *out, size_t out_size,
                                ime_voice_cp_allowed_fn allowed, size_t *dropped)
{
    size_t o = 0;
    size_t drop = 0;

    if (out == NULL || out_size == 0) {
        if (dropped != NULL) {
            *dropped = 0;
        }
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        if (dropped != NULL) {
            *dropped = 0;
        }
        return 0;
    }
    if (allowed == NULL) {
        allowed = ime_voice_cp_is_basic;
    }

    const unsigned char *p = (const unsigned char *)in;
    while (*p != '\0') {
        /* 先解出这个字符的字节长度与码点；编码坏了就丢掉一个字节继续。 */
        size_t len;
        uint32_t cp;
        if (p[0] < 0x80u) {
            len = 1;
            cp = p[0];
        } else if ((p[0] & 0xE0u) == 0xC0u) {
            len = 2;
            cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        } else if ((p[0] & 0xF0u) == 0xE0u) {
            len = 3;
            cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                 (uint32_t)(p[2] & 0x3Fu);
        } else if ((p[0] & 0xF8u) == 0xF0u) {
            len = 4;
            cp = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
                 ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
        } else {
            p++;
            drop++;
            continue;
        }

        /* 续字节必须是 10xxxxxx，否则这一串是坏的。 */
        bool valid = true;
        for (size_t i = 1; i < len; i++) {
            if ((p[i] & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            p++;
            drop++;
            continue;
        }

        if (allowed(cp)) {
            if (o + len + 1u > out_size) {
                break;                  /* 缓冲满：截断（比返回失败更有用） */
            }
            memcpy(out + o, p, len);
            o += len;
        } else {
            drop++;
        }
        p += len;
    }

    out[o] = '\0';
    if (dropped != NULL) {
        *dropped = drop;
    }
    return o;
}
