/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 键盘表（唯一权威数据源，docs/keymap.md 与之一一对应）。
 *
 * ★ 为什么从 lv_pinyin_ime_kb.c 拆出来：表是**纯数据**，渲染是**控件代码**。分开之后
 *   表可以在 PC 上单独编译、单独校验（test_apps/host_layout_test 会把下面每一张表
 *   都过一遍 ime_layout_validate_keyboard() 并在多种分辨率下求解），而改键位不再需要
 *   看懂 LVGL 那一堆 flex/样式。旧实现把两者混在一个文件里，于是"某一行权重之和是
 *   100 而不是 120"这种问题只能靠肉眼发现 —— 指南里那张"当前权重总和"表就是这么来的。
 *
 * 宽度单位：width_u = 十分之一单位（10 = 1u），含义随行的对齐模式而变：
 *
 *   IME_ROW_ALIGN_CENTER  标准键宽的十分之一。标准键宽 U 由键盘的 ref_row 反算，
 *                         所以同一列在不同行里宽度完全相同（26 键的字母键在 R1/R2/R3
 *                         都是 0.9u），整行按标准键宽排好后居中 —— 第二、三行因此天然
 *                         缩进，这是标准手机键盘的画法。
 *   IME_ROW_ALIGN_FILL    行内相对权重，整行铺满网格宽度。各行权重之和**不必**相等，
 *                         只有同一行内部的比例有意义（例如 20/20/30/25/25）。
 *
 * 每张表都在下面显式写出自己的对齐模式与 ref_row —— 不再让"这一行该不该铺满"这件事
 * 藏在某个宏或者渲染代码的 if 里。
 */

#pragma once

#include "ime_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主键盘：26 键（中/英）、9 键（中/英共用）、语音页 */
extern const ime_keyboard_t ime_kb_k26_cn;
extern const ime_keyboard_t ime_kb_k26_en;
extern const ime_keyboard_t ime_kb_k9;
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
extern const ime_keyboard_t ime_kb_voice;
#endif

/* 面板：P1 数字 + 常用符号（含"其他符号"批次）、P2 纯数字、P3 全部符号 */
extern const ime_keyboard_t ime_kb_p1;
extern const ime_keyboard_t ime_kb_p1_alt;
extern const ime_keyboard_t ime_kb_p2;
extern const ime_keyboard_t ime_kb_p3;

/** One named keyboard, so tests and the dump can sweep every table there is. */
typedef struct {
    const char *name;
    const ime_keyboard_t *kb;
} ime_keymap_entry_t;

/** Every keyboard of the component, in a stable order. */
extern const ime_keymap_entry_t ime_keymaps[];
extern const size_t ime_keymaps_count;

#ifdef __cplusplus
}
#endif
