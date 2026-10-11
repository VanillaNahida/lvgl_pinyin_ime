/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音输入的网络侧接口（示例工程用）。实现在 net_ws.c。
 *
 * 只在 CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y 时参与编译。
 *
 * 这是**示例怎么接**，不是组件 API：组件只要求 4 个回调（见
 * include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h），用别的 ws 库、用 MQTT 桥、用
 * 自研协议都行。这里用 espressif/esp_websocket_client 是因为它是官方维护的托管组件，
 * 且不在 ESP-IDF 核心里 —— 正好符合"网络交给调用方"的分工。
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 建 WebSocket 客户端（不连接）。
 *
 * @param ime 键盘控件；收到返回帧时组件会拿它取会话
 */
esp_err_t demo_ws_init(lv_obj_t *ime);

/** 取组装好的 host 回调表；交给 lv_pinyin_ime_voice_set_config() 用。 */
const lv_pinyin_ime_voice_host_t *demo_ws_host(void);

/**
 * 接上麦克风读函数（mic_i2s.c 提供）。
 *
 * 分开设置是因为麦克风的初始化时机与网络无关：有的板子麦克风要等 codec 上电、
 * 有的要先配 I2S 时钟。组件不关心这个顺序，反正 ws_open 之后才会调 audio_read。
 */
void demo_ws_set_audio(int (*read)(void *user, void *buf, size_t len, uint32_t timeout_ms),
                       void *user);

/** 键盘控件晚于 demo_ws_init() 建立时用它补上。 */
void demo_ws_set_ime(lv_obj_t *ime);

#ifdef __cplusplus
}
#endif
