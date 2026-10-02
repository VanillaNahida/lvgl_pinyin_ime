/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default look of the IME. Everything here is plain LVGL styling so an
 * application can override any object with the usual lv_obj_set_style_* calls;
 * the styles are re-created on request so a theme change is possible at runtime
 * (see lv_pinyin_ime.h).
 */

#include "lv_pinyin_ime_style.h"

#include <string.h>

/*
 * Palette, following the reference layout: white keys with a thin grey outline
 * sitting on a noticeably darker panel, so the key grid reads as separate
 * buttons instead of one flat sheet.
 */
#define IME_COLOR_BG        lv_color_hex(0xD3D8E0)  /* keyboard backing panel */
#define IME_COLOR_KEY       lv_color_hex(0xFFFFFF)
#define IME_COLOR_KEY_EDGE  lv_color_hex(0xB6BDC8)  /* the 1 px key outline */
#define IME_COLOR_KEY_DOWN  lv_color_hex(0xC3CBD8)
#define IME_COLOR_KEY_GREY  lv_color_hex(0xE4E8EE)  /* disabled key fill */
#define IME_COLOR_SPECIAL   lv_color_hex(0xF0F2F6)  /* shift / enter / space */
#define IME_COLOR_ACCENT    lv_color_hex(0x2F6FED)
#define IME_COLOR_TEXT      lv_color_hex(0x1B1D21)
#define IME_COLOR_MUTED     lv_color_hex(0x7A8496)
#define IME_COLOR_CAND_BG   lv_color_hex(0xFFFFFF)
#define IME_COLOR_CHIP_BG   lv_color_hex(0x2F6FED)
#define IME_COLOR_CHIP_TEXT lv_color_hex(0xFFFFFF)

/*
 * Corner radius of a key. A key is about 26 px wide and 36 px tall, and LVGL
 * clamps the radius to half the shorter side, so a larger value (or the pill
 * look of a big radius) turns the key into an ellipse. 4 px gives the rounded
 * square of the reference design.
 */
#define IME_KEY_RADIUS 4

/**
 * Thickness of the outline around a key.
 *
 * 1 px is what the reference layout uses. It is left as a constant because a
 * wider outline eats into the key's interior and pushes the caption off centre.
 */
#define IME_KEY_BORDER 1

static lv_style_t s_key;
static lv_style_t s_key_pressed;
static lv_style_t s_key_special;
static lv_style_t s_key_disabled;
static lv_style_t s_label;
static lv_style_t s_cand_btn;
static lv_style_t s_cand_selected;
static lv_style_t s_chip;
static lv_style_t s_bar;
static bool s_inited;

static void init_common(lv_style_t *style)
{
    lv_style_init(style);
    lv_style_set_border_width(style, 0);
    lv_style_set_pad_all(style, 2);
    lv_style_set_shadow_width(style, 0);
}

/** Every key shares the thin outline that separates it from the panel. */
static void init_key(lv_style_t *style)
{
    init_common(style);
    lv_style_set_radius(style, IME_KEY_RADIUS);
    lv_style_set_border_width(style, IME_KEY_BORDER);
    lv_style_set_border_color(style, IME_COLOR_KEY_EDGE);
    lv_style_set_border_opa(style, LV_OPA_COVER);
}

void ime_style_init(void)
{
    if (s_inited) {
        return;
    }

    /* Keys are square with softly rounded corners, not pills: LVGL clamps the
     * radius to half the smaller side, so anything at or above half the key
     * width turns a 26 x 36 key into an ellipse. 4 px stays a rounded square. */
    init_key(&s_key);
    lv_style_set_bg_color(&s_key, IME_COLOR_KEY);
    lv_style_set_bg_opa(&s_key, LV_OPA_COVER);
    lv_style_set_text_color(&s_key, IME_COLOR_TEXT);

    /* Pressed keys keep the outline; only the fill changes. */
    init_key(&s_key_pressed);
    lv_style_set_bg_color(&s_key_pressed, IME_COLOR_KEY_DOWN);

    init_key(&s_key_special);
    lv_style_set_bg_color(&s_key_special, IME_COLOR_SPECIAL);
    lv_style_set_text_color(&s_key_special, IME_COLOR_TEXT);

    init_key(&s_key_disabled);
    lv_style_set_bg_color(&s_key_disabled, IME_COLOR_KEY_GREY);
    lv_style_set_text_color(&s_key_disabled, IME_COLOR_MUTED);

    lv_style_init(&s_label);
    lv_style_set_text_color(&s_label, IME_COLOR_TEXT);
    lv_style_set_text_font(&s_label, LV_FONT_DEFAULT);

    init_common(&s_cand_btn);
    lv_style_set_bg_opa(&s_cand_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_cand_btn, IME_COLOR_TEXT);
    lv_style_set_pad_hor(&s_cand_btn, 4);
    lv_style_set_pad_ver(&s_cand_btn, 2);

    init_common(&s_cand_selected);
    lv_style_set_bg_color(&s_cand_selected, IME_COLOR_ACCENT);
    lv_style_set_bg_opa(&s_cand_selected, LV_OPA_COVER);
    lv_style_set_text_color(&s_cand_selected, IME_COLOR_CHIP_TEXT);

    init_common(&s_chip);
    lv_style_set_bg_color(&s_chip, IME_COLOR_CHIP_BG);
    lv_style_set_bg_opa(&s_chip, LV_OPA_90);
    lv_style_set_text_color(&s_chip, IME_COLOR_CHIP_TEXT);
    lv_style_set_radius(&s_chip, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_chip, 8);
    lv_style_set_pad_ver(&s_chip, 2);

    init_common(&s_bar);
    lv_style_set_bg_color(&s_bar, IME_COLOR_CAND_BG);
    lv_style_set_bg_opa(&s_bar, LV_OPA_COVER);
    lv_style_set_radius(&s_bar, 0);
    lv_style_set_pad_all(&s_bar, 2);

    s_inited = true;
}

void ime_style_apply_key(lv_obj_t *btn, bool special)
{
    ime_style_init();
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, special ? &s_key_special : &s_key, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_key_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_key_disabled, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, IME_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void ime_style_apply_candidate(lv_obj_t *btn, bool selected)
{
    ime_style_init();
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_cand_btn, LV_PART_MAIN);
    if (selected) {
        lv_obj_add_style(btn, &s_cand_selected, LV_PART_MAIN);
    }
    lv_obj_add_style(btn, &s_key_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
}

void ime_style_apply_chip(lv_obj_t *label)
{
    ime_style_init();
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, &s_chip, LV_PART_MAIN);
}

void ime_style_apply_bar(lv_obj_t *bar)
{
    ime_style_init();
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &s_bar, LV_PART_MAIN);
}

void ime_style_apply_root(lv_obj_t *root)
{
    ime_style_init();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, IME_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 3, LV_PART_MAIN);
}

void ime_style_apply_font(lv_obj_t *obj, const lv_font_t *font)
{
    if (obj == NULL || font == NULL) {
        return;
    }
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
}

lv_color_t ime_style_color_accent(void)
{
    return IME_COLOR_ACCENT;
}

lv_color_t ime_style_color_text(void)
{
    return IME_COLOR_TEXT;
}
