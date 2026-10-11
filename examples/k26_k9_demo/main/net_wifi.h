/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi + SNTP（示例工程用）。实现在 net_wifi.c。
 *
 * 只在 CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y 时参与编译：语音输入是唯一需要联网的
 * 功能，没开语音的工程不该被塞进一个 Wi-Fi 驱动。
 *
 * 这两个函数是"示例怎么接"的一部分，不是组件 API —— 换成你自己的联网方式（以太网、
 * 4G 模组、已有 RTC）完全可以，组件只要求 time() 是对的。
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 以 STA 模式连接 CONFIG_EXAMPLE_WIFI_SSID / _PASSWORD，等待拿到 IP。
 *
 * 断线会自动重连（语音是"用户按了才用"的功能，网络晚点通没关系）。
 *
 * @param timeout_ms 等待拿到 IP 的上限
 * @return ESP_OK 拿到 IP；ESP_ERR_TIMEOUT 超时（不致命，界面会提示连接失败）
 */
esp_err_t demo_wifi_connect(uint32_t timeout_ms);

/** 启动 SNTP 周期对时。可重复调用。 */
void demo_sntp_start(void);

/** 时间是否已经对上（语音识别的前提条件）。 */
bool demo_time_synced(void);

#ifdef __cplusplus
}
#endif
