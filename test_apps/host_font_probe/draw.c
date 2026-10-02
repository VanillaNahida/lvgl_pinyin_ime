/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Draw a known string with the generated font and dump it as ASCII art.
 *
 * "The glyph exists" and "the glyph draws" are different claims. lv_font_conv
 * output that LVGL 9 cannot decode still reports correct metrics, so this test
 * renders a string and prints the pixels, and does the same with LVGL's own
 * built-in font for comparison. Only the rendering is compared, not the shapes.
 *
 *   cmake -S test_apps/host_font_probe -B build/host_font_probe -G Ninja
 *   ./build/host_font_probe/host_font_probe --draw
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

extern const lv_font_t lv_font_ime_20;
extern const lv_font_t lv_font_ime_16;
extern const lv_font_t lv_font_montserrat_14;

/* Render one string into an alpha buffer and print it as ASCII art. */
static int draw_text(const lv_font_t *font, const char *name, const char *text)
{
    const int w = 160;
    const int h = font->line_height + 4;
    static uint8_t canvas[160 * 64];

    memset(canvas, 0, sizeof(canvas));

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, 0, 0);

    /* Snapshot just the label area. */
    lv_area_t area = {0, 0, w - 1, h - 1};
    lv_draw_buf_t *snap = lv_snapshot_take_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_A8);
    if (snap == NULL) {
        printf("%s: snapshot failed\n", name);
        lv_obj_delete(scr);
        return -1;
    }
    (void)area;
    (void)canvas;

    printf("%s \"%s\":\n", name, text);
    lv_draw_buf_destroy(snap);
    lv_obj_delete(scr);
    return 0;
}

int main(void)
{
    lv_init();
    printf("not implemented yet\n");
    return 0;
}
