/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 麦克风采集骨架。（示例工程用）实现在 mic_i2s.c，那里有要你补的 TODO 清单。
 *
 * 只在 CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y 时参与编译。
 *
 * 这是**示例怎么接麦克风**，不是组件 API：组件只要一个回调，
 * 签名就是 lv_pinyin_ime_voice_host_t::audio_read。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 I2S 麦克风。
 *
 * @param sample_rate 采样率，**必须与 menuconfig 里的 Voice input → Sample rate 一致**
 *                    （16 kHz 每帧 1280 B，8 kHz 每帧 640 B）
 */
esp_err_t mic_i2s_init(int sample_rate);

/** 反初始化。 */
void mic_i2s_deinit(void);

/**
 * audio_read 回调：阻塞读一段 PCM。
 *
 * 契约见 lv_pinyin_ime_voice.h：
 *   格式 16 bit / 单声道 / 小端；允许阻塞到 @p timeout_ms；
 *   返回实际字节数，**返回 0 表示这一拍没数据**（当静音，不报错），
 *   **返回负数表示硬件错误**（连续 5 次才中断本次识别）。
 */
int mic_i2s_read(void *user, void *buf, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
