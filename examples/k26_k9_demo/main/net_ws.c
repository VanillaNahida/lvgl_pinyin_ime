/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音输入的网络侧：把 esp_websocket_client 接到组件的 host 回调上（示例工程用）。
 *
 * 这个文件是 docs/voice.md §2.2 那"4 个回调"的参考实现。它演示了四件容易做错的事：
 *
 *   1. **ws_open 必须等握手完成才返回**。组件拿到 0 之后立刻发第一帧音频；只发起
 *      连接就返回的话，那一帧会丢在还没建立的连接上。
 *
 *   2. **鉴权 URL 每次会话都不同**（带时间戳签名），所以要在 ws_open 里把它交给
 *      ws 客户端。组件的 URL 是**参数**传进来的，别去猜、也别缓存上一次的。
 *
 *   3. **收到文本帧不能直接解析**。回调里 data_ptr/data_len 指的是 ws 库自己的接收
 *      缓冲，既没有结尾 NUL，组件的解析器又需要就地写一个 '\0'（就地解 \uXXXX）。
 *      所以先拷进本地缓冲、补 NUL、再交给组件。
 *
 *   4. **回调跑在网络任务里**，那里绝对不能碰 LVGL。组件对此有准备：
 *      lv_pinyin_ime_voice_ws_message() 只入队，界面更新由组件任务转给 LVGL 线程
 *      （见 src/voice/ime_voice_ui.c 的线程说明）。
 */

#include "net_ws.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

static const char *TAG = "demo_ws";

#define WS_CONNECTED_BIT BIT0

/** 接收缓冲：与组件内部的 IME_VOICE_MAX_FRAME 对齐（更长的帧组件也会丢）。 */
#define WS_RX_MAX 4096

/** 等握手完成的超时。 */
#define WS_OPEN_TIMEOUT_MS 10000

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    lv_obj_t *ime;              /* 键盘控件句柄（只用于取会话） */
    int (*audio_read)(void *user, void *buf, size_t len, uint32_t timeout_ms);
    void *audio_user;
    char rx[WS_RX_MAX + 1];     /* 收帧拷贝缓冲（+1 给解析器就地写 NUL） */
    bool closing;               /* 我们主动关的：不要上报给组件 */
} ws_ctx_t;

static ws_ctx_t s_ws;

/* ---------------------------------------------------------------- 事件回调 */

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    ws_ctx_t *ctx = (ws_ctx_t *)arg;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        xEventGroupSetBits(ctx->events, WS_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupClearBits(ctx->events, WS_CONNECTED_BIT);
        /* 我们主动关的（会话正常结束）不算异常，别打扰组件。 */
        if (!ctx->closing && ctx->ime != NULL) {
            lv_pinyin_ime_voice_ws_closed(lv_pinyin_ime_voice_of(ctx->ime), 0);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        /*
         * 只处理"完整的文本帧"：
         *   0x01 = 文本；0x00 = 分片的后续帧（本示例不拼）；0x08/0x09/0x0A = 控制帧。
         * 控制帧的 data 不是 JSON，交给组件只会换来一条"无法解析"的警告。
         */
        if (ev->op_code != 0x01) {
            break;
        }
        if (ev->payload_len != 0 && (size_t)ev->data_len < (size_t)ev->payload_len) {
            /*
             * 分片或超过接收缓冲。讯飞的返回帧是几百字节的 JSON，正常到不了这里；
             * 真到了就明确报出来，而不是把半截 JSON 喂给解析器 —— 那样只有"无法
             * 解析"的噪音日志，真正的原因被盖住。
             */
            ESP_LOGW(TAG, "fragmented frame (%d of %d bytes) not handled",
                     (int)ev->data_len, (int)ev->payload_len);
            break;
        }
        if (ev->data_len <= 0 || (size_t)ev->data_len > WS_RX_MAX) {
            break;
        }

        /* 拷出来再交出去：见文件头第 3 条。 */
        memcpy(ctx->rx, ev->data_ptr, (size_t)ev->data_len);
        ctx->rx[ev->data_len] = '\0';
        if (ctx->ime != NULL) {
            lv_pinyin_ime_voice_ws_message(lv_pinyin_ime_voice_of(ctx->ime), ctx->rx,
                                           (size_t)ev->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        xEventGroupClearBits(ctx->events, WS_CONNECTED_BIT);
        if (!ctx->closing && ctx->ime != NULL) {
            lv_pinyin_ime_voice_ws_closed(lv_pinyin_ime_voice_of(ctx->ime), -1);
        }
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- host 回调 */

static int host_ws_open(void *user, const char *url)
{
    ws_ctx_t *ctx = (ws_ctx_t *)user;
    if (ctx->client == NULL || url == NULL) {
        ESP_LOGE(TAG, "not configured");
        return -1;
    }

    ctx->closing = false;
    xEventGroupClearBits(ctx->events, WS_CONNECTED_BIT);

    /*
     * 每次会话换地址：组件的 URL 带时间戳签名，一次一签。
     *
     * 说明：esp_websocket_client_set_uri() 只能在客户端**未启动**时调用，而组件正好
     * 是在每次会话开始、连接之前调 ws_open —— 时机是匹配的。
     *
     * 版本提示：config 里的 uri 字段与 set_uri 的参数在不同版本里 const 与否不一样。
     * 这里做了显式转换，两个版本都能编过；升级 ws 组件后如果报丢弃 const 的警告，
     * 说明新版本已经是 const 了，把转换去掉即可。
     */
    if (esp_websocket_client_set_uri(ctx->client, (char *)url) != ESP_OK) {
        ESP_LOGE(TAG, "set_uri failed");
        return -1;
    }

    if (esp_websocket_client_start(ctx->client) != ESP_OK) {
        ESP_LOGE(TAG, "start failed");
        return -1;
    }

    /* ★ 等握手完成（见文件头第 1 条）。 */
    const EventBits_t bits = xEventGroupWaitBits(ctx->events, WS_CONNECTED_BIT, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(WS_OPEN_TIMEOUT_MS));
    if ((bits & WS_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "handshake timed out");
        ctx->closing = true;
        esp_websocket_client_stop(ctx->client);
        return -1;
    }
    return 0;
}

static int host_ws_send(void *user, const char *json)
{
    ws_ctx_t *ctx = (ws_ctx_t *)user;
    if (ctx->client == NULL || json == NULL) {
        return -1;
    }

    /*
     * timeout 传 0：这一帧发不出去就立刻返回错误（组件的状态机会转 ERROR），
     * 而不是堵住 40 ms 的音频节拍。
     *
     * 版本提示：send_text 的参数在新版本里是 const char*，旧版本是 char*。
     * 与 set_uri 一样做了显式转换，两边都能编过。
     */
    const int sent = esp_websocket_client_send_text(ctx->client, (char *)json,
                                                    (int)strlen(json), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "send failed");
        return -1;
    }
    return 0;
}

static void host_ws_close(void *user)
{
    ws_ctx_t *ctx = (ws_ctx_t *)user;
    if (ctx->client == NULL) {
        return;
    }
    /*
     * 要能容忍"还没连上就关"：组件在 ws_open 失败、用户取消、出错收尾时都会调一次。
     * esp_websocket_client_stop() 对没启动的客户端是安全的（内部判了状态）。
     */
    ctx->closing = true;
    esp_websocket_client_stop(ctx->client);
    xEventGroupClearBits(ctx->events, WS_CONNECTED_BIT);
}

static int host_mic_read(void *user, void *buf, size_t len, uint32_t timeout_ms)
{
    ws_ctx_t *ctx = (ws_ctx_t *)user;
    if (ctx->audio_read == NULL) {
        /*
         * 麦克风还没接上。返回 -1 会让状态机转 ERROR 并在状态条上报"麦克风读取失败"
         * —— 比返回 0（当成静音）好：后者会认真地把一分钟的静音推给云端，用户只会
         * 看到"没识别出任何东西"，问题被掩盖。
         */
        ESP_LOGE(TAG, "microphone not attached (see mic_i2s.c TODOs)");
        return -1;
    }
    return ctx->audio_read(ctx->audio_user, buf, len, timeout_ms);
}

/* ---------------------------------------------------------------- 公开 */

esp_err_t demo_ws_init(lv_obj_t *ime)
{
    if (s_ws.client != NULL) {
        s_ws.ime = ime;
        return ESP_OK;
    }

    s_ws.events = xEventGroupCreate();
    if (s_ws.events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ws.ime = ime;

    /*
     * 几个字段的说明（写这份示例时用的是 esp_websocket_client 1.8.0，
     * 依赖只要求 idf >= 5.0）：
     *
     *   .uri                连接地址。这里只是占位：真正的地址每次会话都由组件签好、
     *                       在 ws_open 里通过 set_uri 交进来。
     *   .crt_bundle_attach  TLS 根证书用 ESP-IDF 自带的证书包。**不要**为了省事关掉
     *                       证书校验：那等于给中间人开门，而语音内容是要上云的。
     *   .disable_auto_reconnect
     *                       关掉自动重连：一次识别结束就该断开，自动重连只会让服务端
     *                       多收一次无效握手。
     *   .task_stack         事件回调里要做 memcpy + 调组件，4 KB 偏紧。
     *   .buffer_size        接收缓冲，要放得下一帧 JSON（讯飞返回帧几百字节到 2 KB）。
     */
    const esp_websocket_client_config_t cfg = {
        .uri = "wss://iat-api.xfyun.cn/v2/iat",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .task_stack = 6144,
        .buffer_size = WS_RX_MAX,
    };

    s_ws.client = esp_websocket_client_init(&cfg);
    if (s_ws.client == NULL) {
        ESP_LOGE(TAG, "init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws.client, WEBSOCKET_EVENT_ANY, ws_event_cb, &s_ws);
    ESP_LOGI(TAG, "websocket client ready (cert bundle on, auto-reconnect off)");
    return ESP_OK;
}

const lv_pinyin_ime_voice_host_t *demo_ws_host(void)
{
    static const lv_pinyin_ime_voice_host_t host = {
        .user = &s_ws,
        .ws_open = host_ws_open,
        .ws_send_text = host_ws_send,
        .ws_close = host_ws_close,
        .audio_read = host_mic_read,
    };
    return &host;
}

void demo_ws_set_audio(int (*read)(void *user, void *buf, size_t len, uint32_t timeout_ms),
                       void *user)
{
    s_ws.audio_read = read;
    s_ws.audio_user = user;
}

void demo_ws_set_ime(lv_obj_t *ime)
{
    s_ws.ime = ime;
}
