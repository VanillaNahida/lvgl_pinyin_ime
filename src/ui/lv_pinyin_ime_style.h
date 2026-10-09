/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the shared styles. Idempotent. */
void ime_style_init(void);

/** Style a key button; @p special selects the darker modifier look. */
void ime_style_apply_key(lv_obj_t *btn, bool special);

/**
 * Style a candidate button; @p selected highlights the active candidate.
 *
 * @p grow adds flex_grow (the 9-key pinyin chips use it to fill their row).
 * Candidates must pass false: their width is measured per text by the UI, and
 * flex_grow would squash a long phrase down to the average item width.
 */
void ime_style_apply_candidate(lv_obj_t *btn, bool selected, bool grow);

/** Style the floating pinyin chip. */
void ime_style_apply_chip(lv_obj_t *label);

/** Style the candidate bar container. */
void ime_style_apply_bar(lv_obj_t *bar);

/** Style the IME root container. */
void ime_style_apply_root(lv_obj_t *root);

/**
 * Apply a font to an object's main part.
 *
 * The default style uses LV_FONT_DEFAULT; this is how the generated IME fonts
 * (big for candidates, small for key captions) are attached. Passing NULL is a
 * no-op, which is what happens when the font partition has not been flashed.
 */
void ime_style_apply_font(lv_obj_t *obj, const lv_font_t *font);

/** Accent colour, for the 9-key pinyin row highlight. */
lv_color_t ime_style_color_accent(void);

/** Foreground text colour. */
lv_color_t ime_style_color_text(void);

#ifdef __cplusplus
}
#endif
