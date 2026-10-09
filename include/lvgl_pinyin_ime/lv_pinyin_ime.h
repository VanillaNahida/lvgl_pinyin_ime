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
 * Override the height of one key row / candidate row, in pixels.
 *
 * The keyboard is laid out as fixed-height rows, so the widget's total height is
 * row_height x rows (26-key Chinese is 4 key rows + 1 candidate row). The default
 * (IME_UI_ROW_HEIGHT_DEFAULT, 28 px) suits a small screen; on a large panel it
 * makes the keys short and hard to hit, so a caller that has the room should raise
 * this. For "the keyboard takes half of a 480 px screen", 45 px is the value that
 * makes 5 rows fill ~240 px.
 *
 * Like the width unit, the row height is global state read when a row is created,
 * so call this BEFORE lv_pinyin_ime_create() (or before a rebuild) - an existing
 * widget is not re-laid out.
 *
 * @param px  row height in pixels, clamped to [16, 256]; 0 or less restores the
 *            default
 */
void lv_pinyin_ime_set_row_height(int px);

/**
 * Fit the whole keyboard to a target height (pixels), e.g. half the screen.
 *
 * Unlike lv_pinyin_ime_set_row_height(), this does not ask for a fixed row size:
 * the widget derives the row height from the target height and the *visible* row
 * count, so it keeps working when the layout changes (26-key vs 9-key, panels,
 * the 选拼音 row shown/hidden):
 *
 *   row_height = (fit - padding/gaps) / visible_rows
 *
 * Keys are already flex-sized from the parent width, so the keys scale to fill
 * the fit box in both dimensions. Call it BEFORE lv_pinyin_ime_create() (it is
 * read at create time and re-applied on every rebuild).
 *
 * @param px  target widget height in pixels, clamped to [96, 720]; 0 or less
 *            disables fitting and returns to fixed rows
 */
void lv_pinyin_ime_set_fit_height(int px);

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