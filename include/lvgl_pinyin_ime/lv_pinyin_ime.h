/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a pinyin IME widget.
 *
 * @param parent  parent object, usually `lv_screen_active()`
 * @return the IME root object
 */
lv_obj_t *lv_pinyin_ime_create(lv_obj_t *parent);

/**
 * Bind a text area as the commit target.
 *
 * @param ime  IME root object
 * @param ta   text area receiving committed text, or NULL to detach
 */
void lv_pinyin_ime_attach(lv_obj_t *ime, lv_obj_t *ta);

/** Set the keyboard mode (26-key / 9-key). */
void lv_pinyin_ime_set_mode(lv_obj_t *ime, lv_pinyin_ime_mode_t mode);

/** Get the current keyboard mode. */
lv_pinyin_ime_mode_t lv_pinyin_ime_get_mode(lv_obj_t *ime);

/** Set the input language (Chinese pinyin / English). */
void lv_pinyin_ime_set_lang(lv_obj_t *ime, lv_pinyin_ime_lang_t lang);

/** Get the current input language. */
lv_pinyin_ime_lang_t lv_pinyin_ime_get_lang(lv_obj_t *ime);

/** Switch the visible panel (P0 main / P1 numeric+symbol / P2 numeric / P3 all symbols). */
void lv_pinyin_ime_set_panel(lv_obj_t *ime, lv_pinyin_ime_panel_t panel);

/** Get the visible panel. */
lv_pinyin_ime_panel_t lv_pinyin_ime_get_panel(lv_obj_t *ime);

/** Clear the pinyin buffer, the candidate list and the selection cursor. */
void lv_pinyin_ime_reset(lv_obj_t *ime);

/** Show and restore the IME. */
void lv_pinyin_ime_show(lv_obj_t *ime);

/** Hide the IME. */
void lv_pinyin_ime_hide(lv_obj_t *ime);

/**
 * Log the widget state: root coordinates, mode, whether the engine and the
 * fonts loaded, and the geometry of every key row.
 *
 * Written for bring-up on real hardware, where the interesting question is
 * "did the keyboard get built, and where is it?".
 */
void lv_pinyin_ime_dump(lv_obj_t *ime);

#ifdef __cplusplus
}
#endif