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
static lv_style_t s_key_active;
static lv_style_t s_key_disabled;
static lv_style_t s_label;
static lv_style_t s_cand_btn;
static lv_style_t s_cand_grow;
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

    /*
     * Pressed keys keep the outline; only the fill changes.
     *
     * ★ bg_opa must be set here even though the bases already have it: a style
     *   only carries the properties it defines, and the candidate buttons sit on
     *   a base whose bg_opa is TRANSP - so "pressed" only changed the colour of
     *   a fully transparent background and candidates/chips had no press
     *   feedback at all.
     */
    init_key(&s_key_pressed);
    lv_style_set_bg_color(&s_key_pressed, IME_COLOR_KEY_DOWN);
    lv_style_set_bg_opa(&s_key_pressed, LV_OPA_COVER);

    init_key(&s_key_special);
    lv_style_set_bg_color(&s_key_special, IME_COLOR_SPECIAL);
    lv_style_set_text_color(&s_key_special, IME_COLOR_TEXT);

    /*
     * Latched key (LV_STATE_CHECKED): the shift key while caps lock is on. The
     * accent fill is the same blue the candidate bar uses for the selected
     * candidate, so "this key is currently doing something" reads the same
     * everywhere in the widget.
     */
    init_key(&s_key_active);
    lv_style_set_bg_color(&s_key_active, IME_COLOR_ACCENT);
    lv_style_set_bg_opa(&s_key_active, LV_OPA_COVER);
    lv_style_set_border_color(&s_key_active, IME_COLOR_ACCENT);
    lv_style_set_text_color(&s_key_active, IME_COLOR_CHIP_TEXT);

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

    /*
     * 把一行的剩余宽度平分给这几个对象（九键的拼音 chip 用）。
     *
     * ⚠ **候选词不能用它**：候选宽度是 UI 按每个词的实际字宽量出来、再平分剩余
     *   宽度定死的（cand.c 的 cand_measure()），而 flex_grow 的"基准宽度"取的是
     *   子对象**当前**宽度 —— 刚换过文字的按钮基准还是旧值，于是 140 px 的长词
     *   和 20 px 的单字被摊成一样宽，长词按钮装不下自己的文字，标签就溢到邻居上
     *   （用户报的"候选字显示范围外"就是这个）。
     *   放在样式里而不是 lv_obj_set_flex_grow()：ime_style_apply_candidate() 开头
     *   的 lv_obj_remove_style_all() 会把本地属性一起清掉。
     */
    init_common(&s_cand_grow);
    lv_style_set_flex_grow(&s_cand_grow, 1);

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
    /*
     * Text colour comes from the style, never from a local lv_obj_set_style_*()
     * call: local styles outrank every added style in LVGL's cascade, whatever
     * the state, so a local colour here would keep the caption dark in the
     * checked (caps lock) state and the highlight would only change the fill.
     *
     * Order matters as well: styles added later win, so the pressed style goes
     * *after* the checked one - otherwise pressing a latched key (caps lock on)
     * would keep showing the latched colour and the press would go unnoticed.
     * The disabled style stays last: it outranks everything.
     */
    lv_obj_add_style(btn, special ? &s_key_special : &s_key, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_key_active, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_style(btn, &s_key_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_key_disabled, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void ime_style_apply_candidate(lv_obj_t *btn, bool selected, bool grow)
{
    ime_style_init();
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_cand_btn, LV_PART_MAIN);
    if (grow) {
        lv_obj_add_style(btn, &s_cand_grow, LV_PART_MAIN);
    }
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
