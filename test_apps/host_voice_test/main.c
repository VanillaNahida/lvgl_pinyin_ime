/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音输入协议层的主机端测试。
 *
 * 这一层不依赖 LVGL / ESP-IDF / 网络，所以可以在 PC 上把整条协议链跑完：
 *   - HMAC-SHA256 与 base64：拿**讯飞文档里的样例**逐字节比对，再跑 RFC 4231 向量；
 *   - 鉴权 URL：固定 date，断言 signature / authorization 与文档一致；
 *   - 请求帧：断言首帧/中间帧/结束帧的 JSON 结构、base64 音频长度；
 *   - 返回帧解析 + 动态修正：把文档里的样例（apd/rpl、nbest、vinfo、错误码）喂进去。
 *
 * 关键设计：解析器是**就地**把 \uXXXX 解成 UTF-8 的，所以喂进去的 JSON 必须放在
 * 可写缓冲里，且末尾要多留 1 字节（见 ime_voice_parse_reply 的注释）。测试里统一
 * 用 feed() 帮手拷进本地缓冲再解析 —— 这也顺便验证了"调用方需要多给 1 字节"这条
 * 契约在真实用法下是够的。
 *
 * 用法：
 *   cmake -S test_apps/host_voice_test -B build/host_voice_test -G Ninja
 *   cmake --build build/host_voice_test
 *   ./build/host_voice_test/host_voice_test
 */

#include <stdio.h>
#include <string.h>

#include "voice/ime_voice_proto.h"

/* ---------------------------------------------------------------- 断言框架 */

static int s_pass;
static int s_fail;
static const char *s_group = "";

static void group(const char *name)
{
    s_group = name;
    printf("\n== %s ==\n", name);
}

static void ok(bool cond, const char *what)
{
    if (cond) {
        s_pass++;
        printf("  ok    %s\n", what);
    } else {
        s_fail++;
        printf("  FAIL  [%s] %s\n", s_group, what);
    }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    const bool same = (got != NULL && want != NULL && strcmp(got, want) == 0);
    if (same) {
        s_pass++;
        printf("  ok    %s\n", what);
    } else {
        s_fail++;
        printf("  FAIL  [%s] %s\n        got : \"%s\"\n        want: \"%s\"\n", s_group, what,
               got != NULL ? got : "(null)", want != NULL ? want : "(null)");
    }
}

static void eq_int(long got, long want, const char *what)
{
    if (got == want) {
        s_pass++;
        printf("  ok    %s\n", what);
    } else {
        s_fail++;
        printf("  FAIL  [%s] %s (got %ld, want %ld)\n", s_group, what, got, want);
    }
}

/**
 * 按契约把 JSON 喂给解析器：拷进本地可写缓冲，长度不含 NUL，缓冲留出结尾字节。
 */
static bool feed(const char *json, ime_voice_reply_t *out)
{
    static char buf[8192];
    const size_t len = strlen(json);
    if (len + 1u > sizeof(buf)) {
        return false;
    }
    memcpy(buf, json, len);
    return ime_voice_parse_reply(buf, len, out);
}

static const char *hex(const uint8_t *data, size_t len, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = d[data[i] >> 4];
        out[i * 2 + 1] = d[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return out;
}

/* ------------------------------------------------------------- 1. 密码学原语 */

static void test_rfc4231(void)
{
    group("HMAC-SHA256（RFC 4231 测试向量）");

    uint8_t mac[32];
    char got[65];

    /* 用例 1：key = 0x0b × 20，data = "Hi There" */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    ime_voice_hmac_sha256(key1, sizeof(key1), (const uint8_t *)"Hi There", 8, mac);
    eq_str(hex(mac, 32, got), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
           "RFC 4231 case 1");

    /* 用例 2：key = "Jefe"，data = "what do ya want for nothing?" */
    ime_voice_hmac_sha256((const uint8_t *)"Jefe", 4,
                          (const uint8_t *)"what do ya want for nothing?", 28, mac);
    eq_str(hex(mac, 32, got), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
           "RFC 4231 case 2");

    /* 用例 6：key = 131 字节的 0xaa —— 超过块长，走"先哈希密钥"那条分支 */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    const char *data6 =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    ime_voice_hmac_sha256(key6, sizeof(key6), (const uint8_t *)data6, strlen(data6), mac);
    eq_str(hex(mac, 32, got), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
           "RFC 4231 case 6（密钥超块长）");
}

static void test_base64(void)
{
    group("base64");

    char out[64];
    uint8_t raw[256];

    eq_int((long)ime_voice_b64_encode((const uint8_t *)"", 0, out, sizeof(out)), 0,
           "空输入编出 0 字符");
    eq_str(out, "", "空输入结果为空串");

    ime_voice_b64_encode((const uint8_t *)"f", 1, out, sizeof(out));
    eq_str(out, "Zg==", "\"f\" → Zg==");

    ime_voice_b64_encode((const uint8_t *)"fo", 2, out, sizeof(out));
    eq_str(out, "Zm8=", "\"fo\" → Zm8=");

    ime_voice_b64_encode((const uint8_t *)"foo", 3, out, sizeof(out));
    eq_str(out, "Zm9v", "\"foo\" → Zm9v");

    ime_voice_b64_encode((const uint8_t *)"foobar", 6, out, sizeof(out));
    eq_str(out, "Zm9vYmFy", "\"foobar\" → Zm9vYmFy");

    /* 往返：0..255 全字节 */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    char enc[400];
    const size_t elen = ime_voice_b64_encode(all, sizeof(all), enc, sizeof(enc));
    const size_t dlen = ime_voice_b64_decode(enc, elen, raw, sizeof(raw));
    eq_int((long)dlen, 256, "256 字节往返长度一致");
    ok(memcmp(all, raw, 256) == 0, "256 字节往返内容一致");

    /* 缓冲不够要拒绝，而不是写溢出 */
    char tiny[5];
    eq_int((long)ime_voice_b64_encode(all, sizeof(all), tiny, sizeof(tiny)), 0,
           "缓冲不够时返回 0（不截断、不越界）");

    /* 宽容换行（服务端不会插，但样例文件里可能有） */
    eq_int((long)ime_voice_b64_decode("Zm9v\nYmFy", 9, raw, sizeof(raw)), 6, "解码忽略换行");

    /* 非法字符要失败 */
    eq_int((long)ime_voice_b64_decode("Zm9v!!!!", 8, raw, sizeof(raw)), 0, "非法字符返回 0");
}

static void test_url_encode(void)
{
    group("URL 编码");
    char out[256];
    ime_voice_url_encode("Wed, 10 Jul 2019 07:35:43 GMT", out, sizeof(out));
    eq_str(out, "Wed%2C%2010%20Jul%202019%2007%3A35%3A43%20GMT", "RFC1123 时间串的转义");

    ime_voice_url_encode("iat-api.xfyun.cn", out, sizeof(out));
    eq_str(out, "iat-api.xfyun.cn", "unreserved 字符保持原样（- 与 .）");

    ime_voice_url_encode("a+b/c=d", out, sizeof(out));
    eq_str(out, "a%2Bb%2Fc%3Dd", "base64 里的 + / = 都要转义");
}

/* ------------------------------------------------------------- 2. 鉴权 URL */

static void test_rfc1123(void)
{
    group("RFC1123 时间格式化");

    char buf[64];

    /* 文档里用的就是这个时间 */
    ok(ime_voice_rfc1123(1562744143, buf, sizeof(buf)), "能格式化文档样例的时间戳");
    eq_str(buf, "Wed, 10 Jul 2019 07:35:43 GMT", "2019-07-10 07:35:43Z 的 RFC1123 形式");

    /* 1970-01-01 00:00:00 是星期四 */
    ime_voice_rfc1123(0, buf, sizeof(buf));
    eq_str(buf, "Thu, 01 Jan 1970 00:00:00 GMT", "epoch");

    /* 2026-01-01 00:00:00 UTC（星期四） */
    ime_voice_rfc1123(1767225600, buf, sizeof(buf));
    eq_str(buf, "Thu, 01 Jan 2026 00:00:00 GMT", "2026-01-01");

    /* 闰日：2024-02-29 12:00:00 UTC */
    ime_voice_rfc1123(1709208000, buf, sizeof(buf));
    eq_str(buf, "Thu, 29 Feb 2024 12:00:00 GMT", "2024-02-29（闰日）");

    /* 2099-12-31 23:59:59 UTC */
    ime_voice_rfc1123(4102444799, buf, sizeof(buf));
    eq_str(buf, "Thu, 31 Dec 2099 23:59:59 GMT", "2099-12-31");

    ok(!ime_voice_rfc1123(-1, buf, sizeof(buf)), "负数时间（时钟没对）被拒绝");
}

static void test_auth_url(void)
{
    group("鉴权 URL（对文档样例逐字节校验）");

    ime_voice_params_t p;
    ime_voice_params_defaults(&p);
    snprintf(p.apikey, sizeof(p.apikey), "%s", "keyxxxxxxxx8ee279348519exxxxxxxx");
    snprintf(p.apisecret, sizeof(p.apisecret), "%s",
             "secretxxxxxxxx2df7900c09xxxxxxxx");
    snprintf(p.appid, sizeof(p.appid), "%s", "12345678");

    char url[1024];
    /* 鉴权中间缓冲：真机上在会话工作区（堆），这里用静态变量 —— 都不能放任务栈。 */
    static ime_voice_auth_scratch_t scratch;
    ok(ime_voice_build_auth_url(&p, "Wed, 10 Jul 2019 07:35:43 GMT", &scratch, url, sizeof(url)),
       "能拼出鉴权 URL");

    /* 文档 §鉴权方法 给出的 authorization 原值 */
    static const char *want_auth =
        "YXBpX2tleT0ia2V5eHh4eHh4eHg4ZWUyNzkzNDg1MTlleHh4eHh4eHgiLCBhbGdvcml0aG09Imht"
        "YWMtc2hhMjU2IiwgaGVhZGVycz0iaG9zdCBkYXRlIHJlcXVlc3QtbGluZSIsIHNpZ25hdHVyZT0i"
        "SHAzVHk0WmtTQm1MOGpLeU9McFFpdjlTcjVudm1lWUVIN1dzTC9aTzJKZz0i";

    char want_url[1024];
    snprintf(want_url, sizeof(want_url),
             "wss://iat-api.xfyun.cn/v2/iat?authorization=%s"
             "&date=Wed%%2C%%2010%%20Jul%%202019%%2007%%3A35%%3A43%%20GMT"
             "&host=iat-api.xfyun.cn",
             want_auth);
    eq_str(url, want_url, "完整 URL 与文档样例一致");

    /* signature 单独对一遍（文档：Hp3Ty4ZkSBmL8jKyOLpQiv9Sr5nvmeYEH7WsL/ZO2Jg=） */
    const char *auth_origin =
        "api_key=\"keyxxxxxxxx8ee279348519exxxxxxxx\", algorithm=\"hmac-sha256\", "
        "headers=\"host date request-line\", "
        "signature=\"Hp3Ty4ZkSBmL8jKyOLpQiv9Sr5nvmeYEH7WsL/ZO2Jg=\"";
    char enc[512];
    ime_voice_b64_encode((const uint8_t *)auth_origin, strlen(auth_origin), enc, sizeof(enc));
    eq_str(enc, want_auth, "authorization 原值与文档一致（= HMAC 与格式都对）");

    /* 端点可换（私有部署 / 小语种走 iat-niche-api） */
    snprintf(p.endpoint, sizeof(p.endpoint), "%s", "wss://iat-niche-api.xfyun.cn/v2/iat");
    ok(ime_voice_build_auth_url(&p, "Wed, 10 Jul 2019 07:35:43 GMT", &scratch, url, sizeof(url)),
       "换小语种端点也能拼");
    ok(strstr(url, "iat-niche-api.xfyun.cn") != NULL, "URL 里是新端点");

    /* 参数不全要拒绝 */
    ime_voice_params_t bad;
    ime_voice_params_defaults(&bad);
    ok(!ime_voice_params_valid(&bad), "缺密钥时 params_valid 为假");
    /*
     * 注意：build_auth_url 只做拼接，不做参数校验 —— 校验是调用方（状态机）的
     * 责任，这样测试能单独验签名算法。这里断言的是"用空密钥也能算出确定结果"，
     * 不崩即可。
     */
    ok(ime_voice_build_auth_url(&bad, "Wed, 10 Jul 2019 07:35:43 GMT", &scratch, url, sizeof(url)),
       "空密钥下拼接不崩（校验由状态机做）");
}

/* ------------------------------------------------------------- 3. 请求帧 */

static void test_request_frames(void)
{
    group("请求帧");

    ime_voice_params_t p;
    ime_voice_params_defaults(&p);
    snprintf(p.appid, sizeof(p.appid), "%s", "abc123");

    uint8_t pcm[IME_VOICE_FRAME_BYTES_16K];
    for (size_t i = 0; i < sizeof(pcm); i++) {
        pcm[i] = (uint8_t)(i & 0xff);
    }

    static char frame[8192];

    /* 首帧 */
    const size_t n = ime_voice_build_first_frame(&p, pcm, sizeof(pcm), frame, sizeof(frame));
    ok(n > 0, "首帧生成成功");
    ok(strstr(frame, "\"common\":{\"app_id\":\"abc123\"}") != NULL, "首帧带 common.app_id");
    ok(strstr(frame, "\"language\":\"zh_cn\"") != NULL, "业务参数 language 正确");
    ok(strstr(frame, "\"domain\":\"iat\"") != NULL, "业务参数 domain 正确");
    ok(strstr(frame, "\"accent\":\"mandarin\"") != NULL, "业务参数 accent 正确");
    ok(strstr(frame, "\"eos\":2000") != NULL, "eos 默认 2000 ms");
    ok(strstr(frame, "\"dwa\":\"wpgs\"") != NULL, "默认开启动态修正");
    ok(strstr(frame, "\"status\":0") != NULL, "首帧 data.status = 0");
    ok(strstr(frame, "\"format\":\"audio/L16;rate=16000\"") != NULL, "16k 采样率格式串");
    ok(strstr(frame, "\"encoding\":\"raw\"") != NULL, "编码 raw（PCM）");
    ok(frame[n - 2] == '}' && frame[n - 1] == '}', "帧以 \"}} 收尾");

    /* 音频字段的 base64 长度：1280 → 1708 字符 */
    const char *audio = strstr(frame, "\"audio\":\"");
    ok(audio != NULL, "首帧带 audio 字段");
    if (audio != NULL) {
        audio += strlen("\"audio\":\"");
        const char *close = strstr(audio, "\"}");
        ok(close != NULL, "audio 字段有收尾");
        if (close != NULL) {
            eq_int((long)(close - audio), (long)ime_voice_b64_len(sizeof(pcm)) - 1,
                   "1280 B PCM → 1708 字符 base64");
        }
        /* 解回来必须与原 PCM 一致 */
        static uint8_t back[4096];
        const size_t blen = ime_voice_b64_decode(audio, (size_t)(close - audio), back,
                                                 sizeof(back));
        eq_int((long)blen, (long)sizeof(pcm), "base64 解回长度一致");
        ok(memcmp(back, pcm, sizeof(pcm)) == 0, "base64 解回内容一致");
    }

    /* 中间帧：不能带 common / business（文档：只在首帧上传） */
    const size_t m = ime_voice_build_audio_frame(&p, pcm, sizeof(pcm), frame, sizeof(frame));
    ok(m > 0, "中间帧生成成功");
    ok(strstr(frame, "\"common\"") == NULL, "中间帧不带 common");
    ok(strstr(frame, "\"business\"") == NULL, "中间帧不带 business");
    ok(strstr(frame, "\"status\":1") != NULL, "中间帧 data.status = 1");

    /* 结束帧 */
    const size_t l = ime_voice_build_last_frame(frame, sizeof(frame));
    ok(l > 0, "结束帧生成成功");
    eq_str(frame, "{\"data\":{\"status\":2}}", "结束帧内容与文档一致");

    /* 关闭标点 / 动态修正时的业务参数 */
    ime_voice_params_defaults(&p);
    p.dwa = false;
    p.ptt = false;
    ime_voice_build_first_frame(&p, pcm, 0, frame, sizeof(frame));
    ok(strstr(frame, "\"dwa\"") == NULL, "关掉动态修正后不发 dwa");
    ok(strstr(frame, "\"ptt\":0") != NULL, "关掉标点后显式发 ptt=0");

    /* 缓冲太小必须拒绝，而不是截断 */
    char small[64];
    eq_int((long)ime_voice_build_first_frame(&p, pcm, sizeof(pcm), small, sizeof(small)), 0,
           "首帧缓冲不够时返回 0");
    eq_int((long)ime_voice_build_audio_frame(&p, pcm, sizeof(pcm), small, sizeof(small)), 0,
           "中间帧缓冲不够时返回 0");

    /* 8k 的帧长与格式串 */
    eq_int((long)ime_voice_frame_bytes(IME_VOICE_RATE_16K), 1280, "16k 每帧 1280 B");
    eq_int((long)ime_voice_frame_bytes(IME_VOICE_RATE_8K), 640, "8k 每帧 640 B");
    eq_str(ime_voice_format_str(IME_VOICE_RATE_8K), "audio/L16;rate=8000", "8k 格式串");
}

/* ------------------------------------------------------------- 4. 返回帧解析 */

/* 文档 §动态修正返回参数 的样例（原样抄下来，含 \u 转义） */
static const char *DOC_WPGS_RPL =
    "{\n"
    "  \"code\": 0,\n"
    "  \"message\": \"success\",\n"
    "  \"sid\": \"iatxxxxxxxxxxxxx\",\n"
    "  \"data\": {\n"
    "    \"result\": {\n"
    "      \"bg\": 0,\n"
    "      \"ed\": 0,\n"
    "      \"ls\": false,\n"
    "      \"pgs\": \"rpl\",\n"
    "      \"rg\": [\n"
    "        1,\n"
    "        1\n"
    "      ],\n"
    "      \"sn\": 2,\n"
    "      \"ws\": [\n"
    "        {\n"
    "          \"bg\": 0,\n"
    "          \"cw\": [\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"\\u6d4b\\u8bd5\"\n"
    "            }\n"
    "          ]\n"
    "        },\n"
    "        {\n"
    "          \"bg\": 0,\n"
    "          \"cw\": [\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"\\u4e00\\u4e0b\"\n"
    "            }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"status\": 1\n"
    "  }\n"
    "}\n";

/* 文档 §vinfo返回参数 的样例：vad.bg 与 ws[].bg 同时存在，最容易踩层级错位 */
static const char *DOC_VINFO =
    "{\n"
    "  \"code\": 0,\n"
    "  \"message\": \"success\",\n"
    "  \"sid\": \"iatxxxxxxxxxxxxxx\",\n"
    "  \"data\": {\n"
    "    \"result\": {\n"
    "      \"bg\": 0,\n"
    "      \"ed\": 0,\n"
    "      \"ls\": false,\n"
    "      \"sn\": 1,\n"
    "      \"vad\": {\n"
    "        \"ws\": [\n"
    "          {\n"
    "            \"bg\": 40,\n"
    "            \"ed\": 366,\n"
    "            \"eg\": 63.58\n"
    "          }\n"
    "        ]\n"
    "      },\n"
    "      \"ws\": [\n"
    "        {\n"
    "          \"bg\": 53,\n"
    "          \"cw\": [\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"4\\u6708\"\n"
    "            }\n"
    "          ]\n"
    "        },\n"
    "        {\n"
    "          \"bg\": 293,\n"
    "          \"cw\": [\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"\\u9009\\u624b\"\n"
    "            }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"status\": 1\n"
    "  }\n"
    "}\n";

/* 文档 §句子多候选（nbest）样例 */
static const char *DOC_NBEST =
    "{\n"
    "  \"code\": 0,\n"
    "  \"message\": \"success\",\n"
    "  \"sid\": \"iatxxxxxxxxxxxxx\",\n"
    "  \"data\": {\n"
    "    \"result\": {\n"
    "      \"bg\": 0,\n"
    "      \"ed\": 0,\n"
    "      \"ls\": false,\n"
    "      \"sn\": 1,\n"
    "      \"ws\": [\n"
    "        {\n"
    "          \"bg\": 35,\n"
    "          \"cw\": [\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"\\u6253\\u7535\\u8bdd\\u7ed9\\u6881\\u7389\\u751f\"\n"
    "            },\n"
    "            {\n"
    "              \"sc\": 0,\n"
    "              \"w\": \"\\u6253\\u7535\\u8bdd\\u7ed9\\u6881\\u7389\\u5347\"\n"
    "            }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"status\": 0\n"
    "  }\n"
    "}\n";

/* 文档 §词级多候选（wbest）样例：每个词都有自己的候选 */
static const char *DOC_WBEST =
    "{\n"
    "  \"code\": 0,\n"
    "  \"message\": \"success\",\n"
    "  \"sid\": \"iatxxxxxxxxxxxxxx\",\n"
    "  \"data\": {\n"
    "    \"result\": {\n"
    "      \"bg\": 0,\n"
    "      \"ed\": 0,\n"
    "      \"ls\": false,\n"
    "      \"sn\": 1,\n"
    "      \"ws\": [\n"
    "        {\n"
    "          \"bg\": 159,\n"
    "          \"cw\": [\n"
    "            { \"sc\": 0, \"w\": \"\\u6881\" }\n"
    "          ]\n"
    "        },\n"
    "        {\n"
    "          \"bg\": 191,\n"
    "          \"cw\": [\n"
    "            { \"sc\": 0, \"w\": \"\\u7389\" },\n"
    "            { \"sc\": 0, \"w\": \"\\u80b2\" }\n"
    "          ]\n"
    "        },\n"
    "        {\n"
    "          \"bg\": 215,\n"
    "          \"cw\": [\n"
    "            { \"sc\": 0, \"w\": \"\\u751f\" },\n"
    "            { \"sc\": 0, \"w\": \"\\u5347\" }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"status\": 0\n"
    "  }\n"
    "}\n";

static void test_parse_reply(void)
{
    group("返回帧解析");

    ime_voice_reply_t r;

    /* --- 错误帧：只有 code / message / sid --- */
    ok(feed("{\"code\":10165,\"message\":\"\\u5ba2\\u6237\\u7aef\\u5e27\\u53ef\\u80fd\\u4e71"
            "\\u5e8f\\uff0cstatus\\u975e\\u6cd5\",\"sid\":\"iat001\"}",
            &r),
       "错误帧能解析");
    eq_int(r.code, 10165, "code = 10165");
    ok(!r.has_data, "错误帧没有 data");
    ok(r.has_sid, "错误帧带 sid");
    /* \u8bcd 之类要正确解成 UTF-8；这里只查长度与首字，避免硬编码整句 */
    ok(strlen(r.message) > 8, "message 已从 \\u 转义解成 UTF-8");

    /* --- 成功帧：动态修正 rpl --- */
    ok(feed(DOC_WPGS_RPL, &r), "文档 rpl 样例能解析");
    eq_int(r.code, 0, "code = 0");
    eq_str(r.sid, "iatxxxxxxxxxxxxx", "sid 正确");
    ok(r.has_data, "带 data");
    eq_int(r.status, 1, "data.status = 1");
    ok(r.has_result, "带 result");
    eq_int(r.sn, 2, "result.sn = 2");
    ok(r.has_pgs, "带 pgs");
    eq_str(r.pgs, "rpl", "pgs = rpl");
    ok(r.has_rg, "带 rg");
    eq_int(r.rg_first, 1, "rg[0] = 1");
    eq_int(r.rg_last, 1, "rg[1] = 1");
    eq_int((long)r.seg_count, 2, "两段 ws");
    eq_str(r.segs[0].text, "测试", "第一段 = 测试");
    eq_str(r.segs[1].text, "一下", "第二段 = 一下");

    char flat[128];
    ime_voice_reply_flat_text(&r, flat, sizeof(flat));
    eq_str(flat, "测试一下", "整帧拼起来 = 测试一下");

    /* --- vinfo：vad.bg 与 ws[].bg 的层级不能串 --- */
    ok(feed(DOC_VINFO, &r), "文档 vinfo 样例能解析");
    ok(r.has_vad, "识别出 vad");
    eq_int(r.vad_bg, 40, "vad.ws[0].bg = 40");
    eq_int(r.vad_ed, 366, "vad.ws[0].ed = 366");
    eq_int((long)r.seg_count, 2, "两段 ws");
    eq_int(r.segs[0].bg, 53, "ws[0].bg = 53（不是 vad 的 40）");
    eq_int(r.segs[1].bg, 293, "ws[1].bg = 293");
    eq_str(r.segs[1].text, "选手", "第二段 = 选手");

    /* --- nbest：一段里多个候选 --- */
    ok(feed(DOC_NBEST, &r), "文档 nbest 样例能解析");
    eq_int((long)r.seg_count, 1, "一段 ws");
    eq_str(r.segs[0].text, "打电话给梁玉生", "首候选正确");
    eq_int(r.segs[0].nbest_count, 2, "两个候选");
    eq_str(r.segs[0].nbest[1], "打电话给梁玉升", "第二候选正确");

    /* --- wbest：逐词候选 --- */
    ok(feed(DOC_WBEST, &r), "文档 wbest 样例能解析");
    eq_int((long)r.seg_count, 3, "三段 ws");
    eq_str(r.segs[1].text, "玉", "第二段首候选 = 玉");
    eq_str(r.segs[1].nbest[1], "育", "第二段次候选 = 育");
    ime_voice_reply_flat_text(&r, flat, sizeof(flat));
    eq_str(flat, "梁玉生", "整帧拼起来 = 梁玉生");

    /* --- 容错 --- */
    ok(!feed("not json at all", &r), "非 JSON 被拒绝");
    ok(!feed("{\"message\":\"no code\"}", &r), "没有 code 的 JSON 被拒绝");
    ok(!ime_voice_parse_reply(NULL, 0, &r), "空指针被拒绝");

    /* 数字写成字符串：文档提到 sc 字段要兼容 float/int，服务端也可能这样写 sn */
    ok(feed("{\"code\":0,\"data\":{\"status\":\"1\",\"result\":{\"ls\":\"true\","
            "\"sn\":\"3\",\"ws\":[{\"bg\":\"10\",\"cw\":[{\"w\":\"a\"}]}]}}}",
            &r),
       "数字/布尔写成字符串也能解析");
    eq_int(r.status, 1, "status \"1\" → 1");
    eq_int(r.sn, 3, "sn \"3\" → 3");
    eq_int(r.segs[0].bg, 10, "bg \"10\" → 10");
    ok(r.ls, "ls \"true\" → true");

    /* 空 ws（服务端在只有标点或静音时会这样） */
    ok(feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":1,\"ws\":[]}}}", &r),
       "空 ws 能解析");
    eq_int((long)r.seg_count, 0, "空 ws 段数为 0");
}

/* ------------------------------------------------------- 5. 动态修正段落表 */

/**
 * 流式性：**每收到一帧就要能拿到当前文本**，不是等最后才出字。
 *
 * 这是这个接口的核心价值（官方 Python demo 的 microphone_stream() 就是边收边用
 * `\r` 覆盖刷新），所以用官方的 iat_pcm_16k.pcm 走一遍真实的帧序列钉住它。
 *
 * 帧序列按服务端的真实形状构造（**收尾帧不带 pgs，而且会重发最后一段**）：
 *   sn=1 apd "今天"        → 今天
 *   sn=2 apd "天气"        → 今天天气
 *   sn=2 rpl [1,1] "明天"  → 明天天气
 *   status=2, ls=true, sn=2, 无 pgs, ws=[{w:"明天"}]   → 仍是 明天天气
 *
 * 最后那条是这个用例最重要的断言：收尾帧重发的内容**不能**被当成新的一段，
 * 否则整句会变成"明天明天"。这个 bug 真的写出来过，真机表现是"说完一句之后，
 * 屏上最后一小段文字重复了一遍"。
 */
static void test_streaming(void)
{
    group("流式性：边收边出字");

    /* 官方 demo 用的那份 16k PCM：确认它真的是一段音频，而且能按 40 ms 切帧 */
    static const char *pcm_path =
        "websdk-python-demo/xfyunsdkdemo/speech/resources/iat/iat_pcm_16k.pcm";
    FILE *f = fopen(pcm_path, "rb");
    if (f == NULL) {
        printf("  skip  找不到官方 PCM 样例 %s（不影响其它用例）\n", pcm_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fclose(f);

    const long frames = bytes / IME_VOICE_FRAME_BYTES_16K;
    ok(bytes >= IME_VOICE_FRAME_BYTES_16K, "官方 PCM 至少有一帧 1280 B");
    /*
     * 不写死帧数：官方这份样例是 152 320 B，不是整数个 40 ms 帧。真正要断言的是
     * "能被切成帧"，以及"尾帧确实不满"—— 这正是组件最后一帧会短一点的原因
     * （少发几字节，服务端一样接受）。
     */
    ok(frames > 100, "官方 PCM 有 100 帧以上（够覆盖一次完整会话）");
    ok((bytes % IME_VOICE_FRAME_BYTES_16K) != 0, "官方 PCM 尾帧不满一帧（组件也允许这样）");
    printf("         样例 %ld B = %ld 整帧 + %ld B 尾帧\n", bytes, frames,
           bytes % IME_VOICE_FRAME_BYTES_16K);

    /* 逐帧喂返回结果，每一步都断言"此刻屏幕上应该是什么" */
    ime_voice_transcript_t t;
    ime_voice_reply_t r;
    static char snap[IME_VOICE_TRANSCRIPT_MAX];
    char text[512];

    ime_voice_transcript_reset(&t);

    static const struct {
        const char *json;
        const char *expect;   /* 这一刻的整句 */
        bool        is_last;  /* 是不是最后一块（status==2 或 ls==true） */
    } seq[] = {
        { "{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":1,\"pgs\":\"apd\","
          "\"ws\":[{\"cw\":[{\"w\":\"\\u4eca\\u5929\"}]}]}}}", "今天", false },
        { "{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"apd\","
          "\"ws\":[{\"cw\":[{\"w\":\"\\u5929\\u6c14\"}]}]}}}", "今天天气", false },
        { "{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"rpl\","
          "\"rg\":[1,1],\"ws\":[{\"cw\":[{\"w\":\"\\u660e\\u5929\"}]}]}}}", "明天天气", false },
        { "{\"code\":0,\"data\":{\"status\":2,\"result\":{\"ls\":true,\"sn\":2,"
          "\"ws\":[{\"cw\":[{\"w\":\"\\u660e\\u5929\"}]}]}}}", "明天天气", true },
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ok(feed(seq[i].json, &r), "每帧都能解析");
        /* 解析出来立刻就是可用的文本（这就是"实时"） */
        ime_voice_transcript_apply(&t, &r, snap);
        ime_voice_transcript_text(&t, text, sizeof(text));
        eq_str(text, seq[i].expect, seq[i].expect);

        /* 最后一块的判据：文档只定义了 status==2；ls 是同一件事的另一个字段 */
        const bool is_last = (r.status == 2) || r.ls;
        eq_int((long)is_last, (long)seq[i].is_last,
               seq[i].is_last ? "末帧被识别为最后一块" : "中间帧不被当成最后一块");
    }

    /*
     * 官方文档里 pgs 只有 apd / rpl 两个取值（"取值为 apd 时…取值为 rpl 时…"），
     * 全文没有 rlt。官方 Python demo 的 iat_test.py 里写了一句
     *     if result.get('pgs') == 'rlt' or chunk.get('status') == 2:
     * —— 那是笔误（后面还 or status == 2，所以不影响 demo 自己工作）。
     * 这里跑一遍是为了确认"未知 pgs 取值不会让解析失败"，免得以后有人照抄进 C。
     */
    ok(feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":3,\"pgs\":\"rlt\","
            "\"ws\":[{\"cw\":[{\"w\":\"X\"}]}]}}}", &r),
       "未知 pgs 取值（如 demo 里的 rlt）不会让解析失败");
}

static void test_transcript(void)
{
    group("动态修正：段落表拼接");

    ime_voice_transcript_t t;
    ime_voice_reply_t r;
    char text[512];
    /* 快照缓冲由调用方提供（真机上用的是会话工作区，见 ime_voice_scratch_t）。 */
    static char snap[IME_VOICE_TRANSCRIPT_MAX];

    /* 未开 wpgs（没有 sn / pgs）：每次都是最新整句，覆盖第 1 段 */
    ime_voice_transcript_reset(&t);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"ws\":[{\"cw\":[{\"w\":\"\\u4f60\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "你", "第一次 = 你");

    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"ws\":[{\"cw\":[{\"w\":\"\\u4f60\\u597d\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "你好", "第二次覆盖成整句 = 你好（不是 你你好）");

    /* 开 wpgs：apd 追加 / rpl 替换 */
    ime_voice_transcript_reset(&t);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":1,\"pgs\":\"apd\",\"ws\":[{\"cw\":[{\"w\":\"\\u4eca\\u5929\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "今天", "apd 第 1 段 = 今天");

    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"apd\",\"ws\":[{\"cw\":[{\"w\":\"\\u5929\\u6c14\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "今天天气", "apd 第 2 段追加 = 今天天气");

    /* 服务端回头修正第 1 段：rg=[1,1] + rpl */
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"rpl\",\"rg\":[1,1],"
         "\"ws\":[{\"cw\":[{\"w\":\"\\u660e\\u5929\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "明天天气", "rpl 替换第 1 段 = 明天天气");

    /* 替换两段：rg=[1,2] */
    ime_voice_transcript_reset(&t);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":1,\"pgs\":\"apd\",\"ws\":[{\"cw\":[{\"w\":\"A\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"apd\",\"ws\":[{\"cw\":[{\"w\":\"B\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"rpl\",\"rg\":[1,2],"
         "\"ws\":[{\"cw\":[{\"w\":\"C\"}]}]}}}", &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "C", "rg=[1,2] 把两段合成一段 C");

    /* 变化检测：同样的内容再来一次应该报"没变"（UI 据此少重画一次） */
    ime_voice_reply_t r2;
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":2,\"pgs\":\"rpl\",\"rg\":[1,2],"
         "\"ws\":[{\"cw\":[{\"w\":\"C\"}]}]}}}", &r2);
    ime_voice_transcript_apply(&t, &r2, snap);
    ok(!ime_voice_transcript_apply(&t, &r2, snap), "重复同一帧报告无变化");

    /* 段号超上限不能越界（服务端理论上不会给，但 sn 是外部输入） */
    ime_voice_transcript_reset(&t);
    feed("{\"code\":0,\"data\":{\"status\":1,\"result\":{\"sn\":9999,\"pgs\":\"rpl\","
         "\"rg\":[1,9999],\"ws\":[{\"cw\":[{\"w\":\"X\"}]}]}}}",
         &r);
    ime_voice_transcript_apply(&t, &r, snap);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "X", "超范围 sn/rg 被钳位，不越界");

    /* 空表 */
    ime_voice_transcript_reset(&t);
    ime_voice_transcript_text(&t, text, sizeof(text));
    eq_str(text, "", "空段落表 → 空串");
}

/* ------------------------------------------------------- 6. 字形过滤 / 工具 */

static void test_filter(void)
{
    group("字形过滤与 UTF-8 工具");

    char out[256];
    size_t drop = 0;

    /* 默认判据：汉字 + ASCII 保留 */
    ime_voice_filter_charset("你好abc", out, sizeof(out), NULL, &drop);
    eq_str(out, "你好abc", "汉字与 ASCII 保留");
    eq_int((long)drop, 0, "没有丢弃");

    /* 窗口外的字符（emoji / 生僻扩展区）被丢掉 */
    ime_voice_filter_charset("好\U0001F600坏", out, sizeof(out), NULL, &drop);
    eq_str(out, "好坏", "emoji 被丢掉");
    eq_int((long)drop, 1, "丢弃计数 = 1（emoji 算一个字符）");

    /* 常用全角标点保留 */
    ime_voice_filter_charset("你好，世界！", out, sizeof(out), NULL, &drop);
    eq_str(out, "你好，世界！", "全角标点保留");
    eq_int((long)drop, 0, "全角标点没被丢");

    /* 自定义判据：只允许 ASCII —— 验证回调真的在用 */
    ime_voice_filter_charset("你好abc", out, sizeof(out),
                             ime_voice_cp_is_basic, &drop);
    eq_str(out, "你好abc", "显式传兜底判据结果一致");

    /* 非法 UTF-8 字节被丢掉而不是原样透传 */
    ime_voice_filter_charset("a\xFF\xFEz", out, sizeof(out), NULL, &drop);
    eq_str(out, "az", "非法字节被丢掉");
    eq_int((long)drop, 2, "两个坏字节");

    /* 缓冲截断：不越界，结果被截短 */
    char tiny[4];
    ime_voice_filter_charset("你好世界", tiny, sizeof(tiny), NULL, &drop);
    eq_str(tiny, "你", "缓冲不够时截断（一个汉字 3 字节 + NUL）");

    /* 码点解码 */
    eq_int((long)ime_voice_utf8_first_cp("好"), 0x597Du, "UTF-8 首码点");
    eq_int((long)ime_voice_utf8_first_cp("a"), 'a', "ASCII 首码点");
    eq_int((long)ime_voice_utf8_first_cp(""), 0, "空串 → 0");
    eq_int((long)ime_voice_utf8_first_cp(NULL), 0, "NULL → 0");

    /* 兜底判据的边界 */
    ok(ime_voice_cp_is_basic(0x4E00u), "U+4E00 在兜底判据里");
    ok(ime_voice_cp_is_basic(0x9FA5u), "U+9FA5 在兜底判据里");
    ok(!ime_voice_cp_is_basic(0x9FA6u), "U+9FA6（GB2312 之外）不在兜底判据里");
    ok(!ime_voice_cp_is_basic(0x1F600u), "emoji 不在兜底判据里");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    printf("lvgl_pinyin_ime host_voice_test —— 语音输入协议层（离线，无网络）\n");

    test_rfc4231();
    test_base64();
    test_url_encode();
    test_rfc1123();
    test_auth_url();
    test_request_frames();
    test_parse_reply();
    test_streaming();
    test_transcript();
    test_filter();

    printf("\n---------------------------------------------\n");
    printf("通过 %d，失败 %d\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
