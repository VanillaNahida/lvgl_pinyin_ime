/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音听写会话的状态机与任务。
 *
 * 线程模型（这里是整个功能里最容易出错的地方，写清楚）：
 *
 *   ┌─ LVGL 线程（app_main 里跑 lv_timer_handler）────────────────────────┐
 *   │  调 start/stop/cancel、收状态回调、画界面                            │
 *   │  读 state()/text()/error()：只是读结构体字段，加锁后读             │
 *   └───────────────▲──────────────────────────────┬─────────────────────┘
 *                   │ 状态/结果回调（经 lv_async_call 转过来）           │
 *                   │                                                │ start/stop/cancel
 *   ┌───────────────┴──────────────────────────────▼─────────────────────┐
 *   │  组件任务（voice_task）                                            │
 *   │   · 每 40 ms 调 host->audio_read() 拿 PCM，拼帧，host->ws_send_text │
 *   │   · 从队列里取 host 塞进来的返回帧，解析、修正段落表、推进状态      │
 *   └───────────────▲──────────────────────────────┬─────────────────────┘
 *                   │ 返回帧入队                     │ ws_open / ws_send_text / ws_close
 *   ┌───────────────┴──────────────────────────────▼─────────────────────┐
 *   │  调用方的网络任务（esp_websocket_client 等）                        │
 *   │  收到文本帧 → lv_pinyin_ime_voice_ws_message()（只入队，不碰 LVGL） │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * 为什么 ws_message 只入队：它跑在调用方的网络任务里，那里**绝对不能碰 LVGL**。
 * 而返回帧要在 LVGL 线程里上屏、在组件任务里推进状态机，所以统一走队列。
 *
 * 为什么把 40 ms 节拍放在组件而不是调用方：文档明确要求"每次发送音频间隔 40 ms、
 * 字节数为 1280 的整数倍"。让每个调用方自己实现这个节拍，等于把一条协议要求散到
 * 所有接入方；放在这里，调用方的 audio_read 只要"阻塞读 I2S"就行。
 */

#include "voice/ime_voice_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/ime_log.h"
#include "voice/ime_voice_proto.h"

static const char *TAG = "ime_voice";

/* ------------------------------------------------------------------ 宏 */

#ifndef CONFIG_LV_PINYIN_IME_VOICE_ENABLE
#define CONFIG_LV_PINYIN_IME_VOICE_ENABLE 0
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APPID
#define CONFIG_LV_PINYIN_IME_VOICE_APPID ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APIKEY
#define CONFIG_LV_PINYIN_IME_VOICE_APIKEY ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APISECRET
#define CONFIG_LV_PINYIN_IME_VOICE_APISECRET ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT
#define CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT "wss://iat-api.xfyun.cn/v2/iat"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE
#define CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE "zh_cn"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_DOMAIN
#define CONFIG_LV_PINYIN_IME_VOICE_DOMAIN "iat"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_ACCENT
#define CONFIG_LV_PINYIN_IME_VOICE_ACCENT "mandarin"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE
#define CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_EOS_MS
#define CONFIG_LV_PINYIN_IME_VOICE_EOS_MS 2000
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_DWA
#define CONFIG_LV_PINYIN_IME_VOICE_DWA 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION
#define CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW
#define CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE
#define CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE 0
#endif

/*
 * 会话时长上限。
 *
 * 文档：整个会话最多 60 s，超过会报 10114/10014。取 55 s 主动收尾，留 5 s 给最后
 * 一块结果的往返 —— 与其让用户撞上"识别突然失败"，不如我们先把结果拿到手。
 */
#define IME_VOICE_MAX_SESSION_MS 55000u

/** 发出结束帧后等最终结果的时长上限；超时就按已有文本收尾，不再干等。 */
#define IME_VOICE_FINISH_TIMEOUT_MS 6000u

/** 一帧音频的节拍（文档建议值）。 */
#define IME_VOICE_TICK_MS IME_VOICE_FRAME_MS

/* b64：131 K 是 16k 采样率下 5.5 秒的音频，够用且不会让帧长得离谱。 */
#define IME_VOICE_FRAME_MAX_B64 (IME_VOICE_FRAME_BYTES_16K * 2 + 64)
#define IME_VOICE_FIRST_FRAME_MAX (IME_VOICE_FIRST_FRAME_OVERHEAD + IME_VOICE_FRAME_MAX_B64 + 64)

/** 收到但还没处理的返回帧缓冲（每条一帧，约几百字节到 2 KB）。 */
#define IME_VOICE_MAX_FRAME 4096

/* ------------------------------------------------------------------ 类型 */

typedef struct {
    char  *json;                /* 堆上，处理完 free */
    size_t len;
} voice_msg_t;

struct lv_pinyin_ime_voice {
    lv_pinyin_ime_voice_cfg_t cfg;
    ime_voice_params_t        params;    /* 由 cfg 翻译而来，供协议层用 */

    SemaphoreHandle_t lock;
    TaskHandle_t      task;
    bool              task_running;

    volatile lv_pinyin_ime_voice_state_t state;
    volatile lv_pinyin_ime_voice_err_t   err;
    volatile int                         server_code;
    volatile bool                        stop_requested;
    volatile bool                        cancel_requested;
    volatile bool                        ws_open_done;
    volatile bool                        ws_closed_by_peer;

    /* 待处理的返回帧队列（生产者：网络任务；消费者：组件任务） */
    voice_msg_t msgs[IME_VOICE_QUEUE_LEN];
    volatile size_t msg_head;
    volatile size_t msg_tail;
    volatile size_t msg_dropped;

    /* 识别文本 */
    ime_voice_transcript_t transcript;
    char                   text[IME_VOICE_TRANSCRIPT_MAX];
    bool                   result_ready;
    uint32_t               audio_ms;

    /* 上一帧是否失败（连续失败才报错，偶发一次不打断会话） */
    int consecutive_audio_errors;

    /* 回调 */
    lv_pinyin_ime_voice_cb_t state_cb;
    void                    *state_cb_user;
    lv_pinyin_ime_voice_cb_t result_cb;
    void                    *result_cb_user;

    /* 帧缓冲：都在组件任务里用，不需要锁 */
    uint8_t pcm[IME_VOICE_FRAME_BYTES_16K];
    char    frame[IME_VOICE_FIRST_FRAME_MAX];
};

/* ------------------------------------------------------------ 小工具 */

static void lock(lv_pinyin_ime_voice_t *v)
{
    if (v->lock != NULL) {
        xSemaphoreTake(v->lock, portMAX_DELAY);
    }
}

static void unlock(lv_pinyin_ime_voice_t *v)
{
    if (v->lock != NULL) {
        xSemaphoreGive(v->lock);
    }
}

/** 由 cfg 翻译出协议层参数。 */
static void cfg_to_params(const lv_pinyin_ime_voice_cfg_t *cfg, ime_voice_params_t *p)
{
    ime_voice_params_defaults(p);
    if (cfg->appid[0] != '\0') {
        snprintf(p->appid, sizeof(p->appid), "%s", cfg->appid);
    }
    if (cfg->apikey[0] != '\0') {
        snprintf(p->apikey, sizeof(p->apikey), "%s", cfg->apikey);
    }
    if (cfg->apisecret[0] != '\0') {
        snprintf(p->apisecret, sizeof(p->apisecret), "%s", cfg->apisecret);
    }
    if (cfg->endpoint[0] != '\0') {
        snprintf(p->endpoint, sizeof(p->endpoint), "%s", cfg->endpoint);
    }
    if (cfg->language[0] != '\0') {
        snprintf(p->language, sizeof(p->language), "%s", cfg->language);
    }
    if (cfg->domain[0] != '\0') {
        snprintf(p->domain, sizeof(p->domain), "%s", cfg->domain);
    }
    if (cfg->accent[0] != '\0') {
        snprintf(p->accent, sizeof(p->accent), "%s", cfg->accent);
    }
    p->rate = (cfg->sample_rate == 8000) ? IME_VOICE_RATE_8K : IME_VOICE_RATE_16K;
    p->dwa = cfg->dynamic_correction;
    p->ptt = cfg->punctuation;
    p->eos_ms = (cfg->eos_ms > 0) ? cfg->eos_ms : 2000;
}

/* ------------------------------------------------------------ 状态与上报 */

static void set_state(lv_pinyin_ime_voice_t *v, lv_pinyin_ime_voice_state_t st)
{
    if (v->state == st) {
        return;
    }
    v->state = st;
    if (v->state_cb != NULL) {
        /* 回调在组件任务里；UI 那边要自己转到 LVGL 线程（见 ime_voice_ui.c）。 */
        v->state_cb(v, v->state_cb_user);
    }
}

/** 按当前错误码更新可读文案（存到 text 之外的静态区，见 state_text）。 */
static const char *err_text(lv_pinyin_ime_voice_err_t e)
{
    switch (e) {
    case LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED:
        return "语音未配置密钥";
    case LV_PINYIN_IME_VOICE_ERR_NO_HOST:
        return "语音未接网络接口";
    case LV_PINYIN_IME_VOICE_ERR_TIME:
        return "系统时间未同步";
    case LV_PINYIN_IME_VOICE_ERR_WS_OPEN:
        return "语音连接失败";
    case LV_PINYIN_IME_VOICE_ERR_WS_SEND:
        return "语音发送失败";
    case LV_PINYIN_IME_VOICE_ERR_AUDIO:
        return "麦克风读取失败";
    case LV_PINYIN_IME_VOICE_ERR_WS_CLOSED:
        return "语音连接已断开";
    case LV_PINYIN_IME_VOICE_ERR_TIMEOUT:
        return "语音识别超时";
    case LV_PINYIN_IME_VOICE_ERR_SERVER:
        return "语音服务返回错误";
    case LV_PINYIN_IME_VOICE_ERR_BUSY:
        return "语音正在识别";
    default:
        return NULL;
    }
}

/**
 * 收尾：关连接、把已识别的文本落地、状态回到 IDLE。
 *
 * ★ 必须由组件任务调用（会 join 自己、会动 transcript）。
 */
static void finish_session(lv_pinyin_ime_voice_t *v, lv_pinyin_ime_voice_err_t err,
                           int server_code)
{
    if (v->cfg.host != NULL && v->cfg.host->ws_close != NULL) {
        v->cfg.host->ws_close(v->cfg.host->user);
    }

    lock(v);
    v->err = err;
    v->server_code = server_code;
    /* 取消的话连已经收到的部分也丢掉（用户按的是"取消"）。 */
    if (v->cancel_requested) {
        memset(v->transcript.parts, 0, sizeof(v->transcript.parts));
        v->transcript.count = 0;
        v->text[0] = '\0';
        v->result_ready = false;
    } else {
        ime_voice_transcript_text(&v->transcript, v->text, sizeof(v->text));
        v->result_ready = (v->text[0] != '\0');
    }
    unlock(v);

    set_state(v, (err == LV_PINYIN_IME_VOICE_ERR_NONE) ? LV_PINYIN_IME_VOICE_DONE
                                                      : LV_PINYIN_IME_VOICE_ERROR);

    if (v->result_cb != NULL && v->result_ready) {
        v->result_cb(v, v->result_cb_user);
    }

    IME_LOGI(TAG, "session finished: err=%d server=%d text=\"%s\" audio=%ums",
             (int)err, server_code, v->text, (unsigned)v->audio_ms);
}

/* ------------------------------------------------------------ 消息队列 */

static void queue_push(lv_pinyin_ime_voice_t *v, const char *json, size_t len)
{
    if (json == NULL || len == 0 || len > IME_VOICE_MAX_FRAME) {
        return;
    }
    lock(v);
    const size_t next = (v->msg_tail + 1u) % IME_VOICE_QUEUE_LEN;
    if (next == v->msg_head) {
        /* 队列满：丢最旧的一条，保新的（新的更接近当前状态）。 */
        char *old = v->msgs[v->msg_head].json;
        v->msg_head = (v->msg_head + 1u) % IME_VOICE_QUEUE_LEN;
        free(old);
        v->msg_dropped++;
    }
    /* 多要一个字节：解析是就地做的，会写 buf[len]。 */
    char *copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        unlock(v);
        return;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    v->msgs[v->msg_tail].json = copy;
    v->msgs[v->msg_tail].len = len;
    v->msg_tail = next;
    unlock(v);
}

/** 取一条待处理的消息（所有权转移给调用者）。 */
static char *queue_pop(lv_pinyin_ime_voice_t *v, size_t *len)
{
    char *json = NULL;
    lock(v);
    if (v->msg_head != v->msg_tail) {
        json = v->msgs[v->msg_head].json;
        *len = v->msgs[v->msg_head].len;
        v->msgs[v->msg_head].json = NULL;
        v->msg_head = (v->msg_head + 1u) % IME_VOICE_QUEUE_LEN;
    }
    unlock(v);
    return json;
}

/* ------------------------------------------------------------ 返回帧处理 */

/**
 * 处理一条返回帧。返回 true 表示会话该结束了。
 *
 * @param final_out 服务端说这是最后一块
 */
static bool handle_reply(lv_pinyin_ime_voice_t *v, char *json, size_t len, bool *final_out)
{
    ime_voice_reply_t r;
    *final_out = false;

    if (!ime_voice_parse_reply(json, len, &r)) {
        IME_LOGW(TAG, "unparsable reply (%u bytes), ignored", (unsigned)len);
        return false;
    }

    if (r.code != 0) {
        IME_LOGE(TAG, "server error code=%d message=\"%s\"", r.code, r.message);
        lock(v);
        v->server_code = r.code;
        unlock(v);
        return true;                    /* 服务端报错：立刻收尾 */
    }

    if (!r.has_result) {
        return false;                   /* 握手后的空帧之类，忽略 */
    }

    lock(v);
    const bool changed = ime_voice_transcript_apply(&v->transcript, &r);
    if (changed) {
        ime_voice_transcript_text(&v->transcript, v->text, sizeof(v->text));
    }
    /* data.status == 2 或 result.ls == true 都表示"最后一块"。 */
    const bool last = (r.status == 2) || r.ls;
    unlock(v);

    if (last) {
        *final_out = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ 组件任务 */

static void voice_task(void *arg)
{
    lv_pinyin_ime_voice_t *v = (lv_pinyin_ime_voice_t *)arg;
    const lv_pinyin_ime_voice_host_t *host = v->cfg.host;

    /* --- 1. 鉴权 + 连接 --- */
    int64_t now = (int64_t)time(NULL);
    if (now < 1600000000) {             /* 2020-09 之前 = 时钟没对上 */
        IME_LOGE(TAG, "system clock is not set (%lld); run SNTP before voice input",
                 (long long)now);
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_TIME, 0);
        goto done;
    }

    if (!ime_voice_build_auth_url_now(&v->params, now, v->frame, sizeof(v->frame))) {
        IME_LOGE(TAG, "cannot build the auth url");
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_OPEN, 0);
        goto done;
    }

    IME_LOGI(TAG, "connecting: %s", v->params.endpoint);
    if (host->ws_open(host->user, v->frame) != 0) {
        IME_LOGE(TAG, "ws_open failed");
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_OPEN, 0);
        goto done;
    }
    v->ws_open_done = true;
    set_state(v, LV_PINYIN_IME_VOICE_LISTENING);

    /* --- 2. 推音频 + 收结果 --- */
    const size_t frame_bytes = ime_voice_frame_bytes(v->params.rate);
    uint32_t elapsed_ms = 0;
    uint32_t finish_deadline_ms = 0;
    bool     sent_last = false;
    bool     server_final = false;
    bool     forced_stop = false;

    while (true) {
        const uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* 2a. 先把队列里的返回帧处理掉 */
        size_t mlen = 0;
        char *msg = NULL;
        while ((msg = queue_pop(v, &mlen)) != NULL) {
            bool last = false;
            const bool stop_now = handle_reply(v, msg, mlen, &last);
            free(msg);
            if (last) {
                server_final = true;
            }
            if (stop_now) {
                if (v->server_code != 0) {
                    finish_session(v, LV_PINYIN_IME_VOICE_ERR_SERVER, v->server_code);
                    goto done;
                }
                /* 服务端主动结束（正常或 status==2）：收尾 */
                if (!sent_last) {
                    const size_t n = ime_voice_build_last_frame(v->frame, sizeof(v->frame));
                    if (n > 0) {
                        (void)host->ws_send_text(host->user, v->frame);
                    }
                    sent_last = true;
                }
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
                goto done;
            }
        }

        /* 2b. 对端断了：如果已经拿到最终结果就当正常结束，否则报错 */
        if (v->ws_closed_by_peer) {
            if (server_final) {
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
            } else {
                IME_LOGW(TAG, "connection closed by peer before the final result");
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_CLOSED, 0);
            }
            goto done;
        }

        /* 2c. 取消优先级最高 */
        if (v->cancel_requested) {
            IME_LOGI(TAG, "cancelled by the user");
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
            goto done;
        }

        /* 2d. 到点了（用户点结束 / 会话超 55 s）：发结束帧，转 FINISHING */
        const bool time_limit = (elapsed_ms >= IME_VOICE_MAX_SESSION_MS);
        if ((v->stop_requested || time_limit) && !sent_last) {
            if (time_limit) {
                IME_LOGW(TAG, "session reached the %u ms limit, stopping",
                         (unsigned)IME_VOICE_MAX_SESSION_MS);
                forced_stop = true;
            }
            const size_t n = ime_voice_build_last_frame(v->frame, sizeof(v->frame));
            if (n == 0 || host->ws_send_text(host->user, v->frame) != 0) {
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
                goto done;
            }
            sent_last = true;
            set_state(v, LV_PINYIN_IME_VOICE_FINISHING);
            finish_deadline_ms = t0 + IME_VOICE_FINISH_TIMEOUT_MS;
        }

        if (sent_last) {
            /* 等最终结果；超时就按已有文本收尾（有文本算成功，没文本算超时）。 */
            const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((int32_t)(now_ms - finish_deadline_ms) >= 0) {
                lock(v);
                const bool have_text = (v->text[0] != '\0');
                unlock(v);
                IME_LOGW(TAG, "no final result within %u ms (have_text=%d)",
                         (unsigned)IME_VOICE_FINISH_TIMEOUT_MS, (int)have_text);
                finish_session(v,
                               have_text ? LV_PINYIN_IME_VOICE_ERR_NONE
                                         : LV_PINYIN_IME_VOICE_ERR_TIMEOUT,
                               0);
                goto done;
            }
            (void)forced_stop;
            vTaskDelay(pdMS_TO_TICKS(IME_VOICE_TICK_MS));
            continue;
        }

        /* 2e. 读一帧麦克风数据并发出去 */
        int got = host->audio_read(host->user, v->pcm, frame_bytes, IME_VOICE_TICK_MS * 2);
        if (got < 0) {
            v->consecutive_audio_errors++;
            /* 偶发一次（I2S 抖动、DMA 欠载）不该打断整段识别，连续失败才报错。 */
            if (v->consecutive_audio_errors >= 5) {
                IME_LOGE(TAG, "audio_read failed %d times in a row, aborting",
                         v->consecutive_audio_errors);
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_AUDIO, 0);
                goto done;
            }
            IME_LOGW(TAG, "audio_read error (retry %d)", v->consecutive_audio_errors);
            got = 0;
        } else {
            v->consecutive_audio_errors = 0;
        }
        if ((size_t)got > frame_bytes) {
            got = (int)frame_bytes;     /* 回调多给了：只发一帧的量，多的丢掉 */
        }

        size_t n;
        if (elapsed_ms == 0) {
            n = ime_voice_build_first_frame(&v->params, v->pcm, (size_t)got, v->frame,
                                            sizeof(v->frame));
        } else {
            n = ime_voice_build_audio_frame(&v->params, v->pcm, (size_t)got, v->frame,
                                            sizeof(v->frame));
        }
        if (n == 0) {
            IME_LOGE(TAG, "frame build failed (%u bytes)", (unsigned)got);
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
            goto done;
        }
        if (host->ws_send_text(host->user, v->frame) != 0) {
            IME_LOGE(TAG, "ws_send_text failed");
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
            goto done;
        }

        elapsed_ms += IME_VOICE_TICK_MS;
        lock(v);
        v->audio_ms = elapsed_ms;
        unlock(v);

        /* 2f. 补足 40 ms 节拍（读 PCM 花掉的时间要从等待里扣掉） */
        const uint32_t t1 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const int32_t spent = (int32_t)(t1 - t0);
        int32_t wait = (int32_t)IME_VOICE_TICK_MS - spent;
        if (wait > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)wait));
        } else {
            vTaskDelay(1);              /* 让出 CPU，避免把 LVGL 饿死 */
        }
    }

done:
    v->task_running = false;
    v->task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ 公开：配置 */

lv_pinyin_ime_voice_cfg_t lv_pinyin_ime_voice_cfg_default(void)
{
    lv_pinyin_ime_voice_cfg_t c;
    memset(&c, 0, sizeof(c));

    snprintf(c.appid, sizeof(c.appid), "%s", CONFIG_LV_PINYIN_IME_VOICE_APPID);
    snprintf(c.apikey, sizeof(c.apikey), "%s", CONFIG_LV_PINYIN_IME_VOICE_APIKEY);
    snprintf(c.apisecret, sizeof(c.apisecret), "%s", CONFIG_LV_PINYIN_IME_VOICE_APISECRET);
    snprintf(c.endpoint, sizeof(c.endpoint), "%s", CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT);
    snprintf(c.language, sizeof(c.language), "%s", CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE);
    snprintf(c.domain, sizeof(c.domain), "%s", CONFIG_LV_PINYIN_IME_VOICE_DOMAIN);
    snprintf(c.accent, sizeof(c.accent), "%s", CONFIG_LV_PINYIN_IME_VOICE_ACCENT);

    c.sample_rate = CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE;
    c.eos_ms = CONFIG_LV_PINYIN_IME_VOICE_EOS_MS;
    c.dynamic_correction = (CONFIG_LV_PINYIN_IME_VOICE_DWA != 0);
    c.punctuation = (CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION != 0);
    c.live_preview = (CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW != 0);
    c.append_result = (CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE == 0);
    c.host = NULL;
    return c;
}

bool lv_pinyin_ime_voice_supported(void)
{
    return (CONFIG_LV_PINYIN_IME_VOICE_ENABLE != 0);
}

lv_pinyin_ime_voice_tap_mode_t lv_pinyin_ime_voice_tap_mode(void)
{
    /* 用宏判断而不是 #if：三个取值都能编译，Kconfig 一改就生效。 */
#if defined(CONFIG_LV_PINYIN_IME_VOICE_TAP_PTT)
    return LV_PINYIN_IME_VOICE_TAP_PTT;
#elif defined(CONFIG_LV_PINYIN_IME_VOICE_TAP_BOTH)
    return LV_PINYIN_IME_VOICE_TAP_BOTH;
#else
    return LV_PINYIN_IME_VOICE_TAP_TOGGLE;
#endif
}

/* ------------------------------------------------------------ 生命周期 */

lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_create(const lv_pinyin_ime_voice_cfg_t *cfg)
{
    /*
     * 这里不需要 #if CONFIG_..._VOICE_ENABLE 保护：整个 src/voice/ 只在开关打开时
     * 才编进组件（见 CMakeLists.txt）。而 lv_pinyin_ime_voice_supported() 仍然保留，
     * 因为界面侧要靠它决定"要不要画语音键"。
     */
    lv_pinyin_ime_voice_t *v = (lv_pinyin_ime_voice_t *)calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->lock = xSemaphoreCreateMutex();
    if (v->lock == NULL) {
        free(v);
        return NULL;
    }

    v->cfg = (cfg != NULL) ? *cfg : lv_pinyin_ime_voice_cfg_default();
    cfg_to_params(&v->cfg, &v->params);
    v->state = LV_PINYIN_IME_VOICE_IDLE;
    return v;
}

void lv_pinyin_ime_voice_destroy(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return;
    }
    /* 先停任务：它对 v 的字段有引用，必须等它退出再释放。 */
    if (v->task_running) {
        v->cancel_requested = true;
        v->stop_requested = true;
        for (int i = 0; i < 200 && v->task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (v->task_running) {
            /* 任务卡在 audio_read 里（调用方的实现没能及时返回）。
             * 不强行删任务：vTaskDelete 掉一个正阻塞在自己的 I2S 读里的任务，
             * 会把调用方的驱动状态搞坏。这里只警告并要求调用方修 timeout。 */
            IME_LOGE(TAG, "voice task did not exit; make audio_read() honour timeout_ms");
        }
    }

    /* 清掉队列里剩下的帧 */
    size_t len = 0;
    char *msg = NULL;
    while ((msg = queue_pop(v, &len)) != NULL) {
        free(msg);
    }

    if (v->lock != NULL) {
        vSemaphoreDelete(v->lock);
    }
    free(v);
}

void lv_pinyin_ime_voice_set_config(lv_pinyin_ime_voice_t *v,
                                    const lv_pinyin_ime_voice_cfg_t *cfg)
{
    if (v == NULL || cfg == NULL) {
        return;
    }
    v->cfg = *cfg;
    cfg_to_params(&v->cfg, &v->params);
}

const lv_pinyin_ime_voice_cfg_t *lv_pinyin_ime_voice_get_config(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? &v->cfg : NULL;
}

void lv_pinyin_ime_voice_set_state_cb(lv_pinyin_ime_voice_t *v,
                                      lv_pinyin_ime_voice_cb_t cb, void *user)
{
    if (v == NULL) {
        return;
    }
    v->state_cb = cb;
    v->state_cb_user = user;
}

void lv_pinyin_ime_voice_set_result_cb(lv_pinyin_ime_voice_t *v,
                                       lv_pinyin_ime_voice_cb_t cb, void *user)
{
    if (v == NULL) {
        return;
    }
    v->result_cb = cb;
    v->result_cb_user = user;
}

/* ------------------------------------------------------------ 控制 */

int lv_pinyin_ime_voice_start(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    if (v->task_running) {
        IME_LOGW(TAG, "already listening");
        return -LV_PINYIN_IME_VOICE_ERR_BUSY;
    }
    if (v->cfg.host == NULL || v->cfg.host->ws_open == NULL ||
        v->cfg.host->ws_send_text == NULL || v->cfg.host->audio_read == NULL) {
        IME_LOGE(TAG, "host callbacks are incomplete");
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NO_HOST;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    if (!ime_voice_params_valid(&v->params)) {
        IME_LOGE(TAG, "AppID / APIKey / APISecret not configured "
                      "(menuconfig -> LVGL Pinyin IME -> Voice input)");
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED;
    }

    /* 复位本次会话的状态 */
    lock(v);
    ime_voice_transcript_reset(&v->transcript);
    v->text[0] = '\0';
    v->result_ready = false;
    v->err = LV_PINYIN_IME_VOICE_ERR_NONE;
    v->server_code = 0;
    v->audio_ms = 0;
    v->msg_head = 0;
    v->msg_tail = 0;
    v->msg_dropped = 0;
    v->stop_requested = false;
    v->cancel_requested = false;
    v->ws_open_done = false;
    v->ws_closed_by_peer = false;
    v->consecutive_audio_errors = 0;
    unlock(v);

    set_state(v, LV_PINYIN_IME_VOICE_CONNECTING);

    v->task_running = true;
    /* 栈：解析 + 拼 2 KB 的帧 + snprintf，4 KB 够；优先级低于 LVGL 所在的主任务
     * （app_main 是 1），避免 40 ms 的节拍把渲染挤掉。 */
    const BaseType_t ok = xTaskCreate(voice_task, "ime_voice", 4096, v, 2, &v->task);
    if (ok != pdPASS) {
        v->task_running = false;
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NO_HOST;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    return 0;
}

void lv_pinyin_ime_voice_stop(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL || !v->task_running) {
        return;
    }
    v->stop_requested = true;
}

void lv_pinyin_ime_voice_cancel(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL || !v->task_running) {
        return;
    }
    v->cancel_requested = true;
    v->stop_requested = true;
}

/* ------------------------------------------------------------ 查询 */

lv_pinyin_ime_voice_state_t lv_pinyin_ime_voice_state(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->state : LV_PINYIN_IME_VOICE_IDLE;
}

lv_pinyin_ime_voice_err_t lv_pinyin_ime_voice_error(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->err : LV_PINYIN_IME_VOICE_ERR_NONE;
}

int lv_pinyin_ime_voice_server_code(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->server_code : 0;
}

const char *lv_pinyin_ime_voice_state_text(const lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return "";
    }
    switch (v->state) {
    case LV_PINYIN_IME_VOICE_CONNECTING:
        return "正在连接";
    case LV_PINYIN_IME_VOICE_LISTENING:
        return "聆听中";
    case LV_PINYIN_IME_VOICE_FINISHING:
        return "识别中";
    case LV_PINYIN_IME_VOICE_DONE:
        return "完成";
    case LV_PINYIN_IME_VOICE_ERROR: {
        const char *t = err_text(v->err);
        if (t == NULL) {
            return "语音出错";
        }
        /* 服务端错误：把错误码一起显示，便于对文档查表。 */
        if (v->err == LV_PINYIN_IME_VOICE_ERR_SERVER && v->server_code != 0) {
            static char buf[48];
            snprintf(buf, sizeof(buf), "语音错误 %d", v->server_code);
            return buf;
        }
        return t;
    }
    case LV_PINYIN_IME_VOICE_IDLE:
    default:
        return "";
    }
}

const char *lv_pinyin_ime_voice_text(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->text : "";
}

bool lv_pinyin_ime_voice_result_ready(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) && v->result_ready;
}

uint32_t lv_pinyin_ime_voice_audio_ms(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->audio_ms : 0;
}

/* ------------------------------------------------------------ 网络回调 */

void lv_pinyin_ime_voice_ws_message(lv_pinyin_ime_voice_t *v, const char *json, size_t len)
{
    if (v == NULL || json == NULL || len == 0) {
        return;
    }
    queue_push(v, json, len);
}

void lv_pinyin_ime_voice_ws_closed(lv_pinyin_ime_voice_t *v, int err)
{
    if (v == NULL) {
        return;
    }
    if (err != 0) {
        IME_LOGW(TAG, "websocket closed with error %d", err);
    }
    v->ws_closed_by_peer = true;
}
