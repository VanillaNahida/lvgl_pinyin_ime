/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 语音键的麦克风图标（由 assets/mic.png 生成，见 tools/gen_mic_icon.py）。
 *
 * 为什么用位图而不是 LVGL 的图标字形：LVGL 自带的 FontAwesome 私用区里没有麦克风
 * 图形（只有 LV_SYMBOL_AUDIO = 喇叭），喇叭表达"播放"而不是"说话"；自带字体里
 * 也不保证有它。位图与字体无关，换了 UI 字体也不会变成方框。
 */
extern const lv_image_dsc_t mic_icon;

#ifdef __cplusplus
}
#endif
