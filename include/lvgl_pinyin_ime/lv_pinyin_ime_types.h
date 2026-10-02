/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Keyboard mode. */
typedef enum {
    LV_PINYIN_IME_MODE_K26 = 0, /**< 26-key full keyboard */
    LV_PINYIN_IME_MODE_K9,      /**< 9-key (T9) keyboard */
    LV_PINYIN_IME_MODE_LAST,
} lv_pinyin_ime_mode_t;

/** Input language. */
typedef enum {
    LV_PINYIN_IME_LANG_CN = 0, /**< Chinese pinyin mode */
    LV_PINYIN_IME_LANG_EN,     /**< English mode */
    LV_PINYIN_IME_LANG_LAST,
} lv_pinyin_ime_lang_t;

/** Double pinyin scheme. */
typedef enum {
    LV_PINYIN_IME_SCHEME_NONE = 0, /**< Full pinyin only */
    LV_PINYIN_IME_SCHEME_XIAOHE,   /**< Xiaohe */
    LV_PINYIN_IME_SCHEME_ZIRANMA,  /**< Ziranma */
    LV_PINYIN_IME_SCHEME_MS,       /**< Microsoft */
    LV_PINYIN_IME_SCHEME_ABC,      /**< Smart ABC */
    LV_PINYIN_IME_SCHEME_ZIGUANG,  /**< Ziguang */
    LV_PINYIN_IME_SCHEME_PYJJ,     /**< Pinyin Jiajia */
    LV_PINYIN_IME_SCHEME_LAST,
} lv_pinyin_ime_scheme_t;

/** Keyboard panel. */
typedef enum {
    LV_PINYIN_IME_PANEL_MAIN = 0, /**< P0: main keyboard */
    LV_PINYIN_IME_PANEL_NUM_SYM,  /**< P1: digits + punctuation + symbols */
    /**
     * P2: kept as its own value for applications that referenced it, but it shows
     * the same "digits + symbols" panel as P1. There used to be a digits-only
     * panel behind a second key; both keys opened the same layout, so the key and
     * the panel were merged into "符/123".
     */
    LV_PINYIN_IME_PANEL_NUM,
    LV_PINYIN_IME_PANEL_ALL_SYM,  /**< P3: all symbols */
    LV_PINYIN_IME_PANEL_LAST,
} lv_pinyin_ime_panel_t;

/**
 * Get the event code sent to the IME object when the user confirms input
 * (candidate committed or the confirm key pressed with an empty buffer).
 */
lv_event_code_t lv_pinyin_ime_event_ready(void);

/** Get the event code sent to the IME object when the candidate list changes. */
lv_event_code_t lv_pinyin_ime_event_cand_changed(void);

#ifdef __cplusplus
}
#endif