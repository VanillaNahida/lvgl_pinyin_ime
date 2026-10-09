/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host render check for the IME widget.
 *
 * Builds the real widget with the real LVGL and renders it into a PNG-style
 * dump (PPM, which read_image/conversion tools handle), then prints the
 * geometry of the objects that should be visible. This is how a layout problem
 * is caught without a board:
 *
 *   cmake -S test_apps/host_ui_render -B build/host_ui_render -G Ninja
 *   cmake --build build/host_ui_render
 *   ./build/host_ui_render/host_ui_render out.ppm [k26|k9|p1|en]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "core/ime_session.h"
#include "data/ime_font.h"
#include "ui/lv_pinyin_ime_internal.h"   /* key tables + the internal context, to drive keys */


/* The host render links the real src/data/ime_dict.c, which reads the dictionary
 * straight from the file system when ESP_PLATFORM is not defined. Set IME_DICT to
 * point it somewhere else.
 *
 * src/data/ime_font.c is linked too: with -DIME_HOST_REAL_FONT it loads the
 * generated IME fonts, so the render shows real CJK glyphs; without it no font is
 * available and the widget falls back to LV_FONT_DEFAULT. */

/** Find a key button by its action, for driving the widget from the test. */
static lv_obj_t *find_key(lv_obj_t *ime, ime_key_action_t action)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return NULL;
    }
    uint32_t rows = lv_obj_get_child_count(ctx->kb_rows);
    for (uint32_t r = 0; r < rows; r++) {
        lv_obj_t *row = lv_obj_get_child(ctx->kb_rows, r);
        uint32_t keys = lv_obj_get_child_count(row);
        for (uint32_t k = 0; k < keys; k++) {
            lv_obj_t *btn = lv_obj_get_child(row, k);
            const ime_key_t *key = lv_obj_get_user_data(btn);
            if (key != NULL && key->action == action) {
                return btn;
            }
        }
    }
    return NULL;
}

/** Type a lower-case pinyin/ASCII string through the real key dispatch path. */
static void type_text(lv_obj_t *ime, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\'') {
            ime_session_push_separator();
            continue;
        }
        ime_session_push_letter(*p);
    }
    ime_ui_commit_pending(ime_ctx_get(ime));
    ime_ui_refresh_all(ime_ctx_get(ime));
    lv_obj_update_layout(ime);
}

static uint8_t s_buf[320 * 240 * 4];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    (void)area;
    (void)px_map;
    /* Nothing to do: the snapshot reads the object tree, not the display. */
}

/**
 * Print the text and geometry of every label under @p obj.
 *
 * The rendering font is LV_FONT_DEFAULT (Montserrat) because the host build does
 * not carry the generated IME fonts, so CJK and symbol captions draw as boxes
 * here. This dump is how you tell "the key really says that" from "the glyph is
 * missing": the text below is authoritative, the PPM is only geometry.
 */
static void dump_labels(lv_obj_t *obj, int depth)
{
    uint32_t i;
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_area_t a;
        lv_obj_get_coords(child, &a);

        if (lv_obj_check_type(child, &lv_label_class)) {
            const char *text = lv_label_get_text(child);
            printf("  %*slabel \"%s\" at (%d,%d) %dx%d\n", depth * 2, "", text,
                   (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        } else {
            printf("  %*s%s at (%d,%d) %dx%d\n", depth * 2, "",
                   lv_obj_check_type(child, &lv_button_class) ? "button" : "obj",
                   (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        }
        if (depth < 5) {
            dump_labels(child, depth + 1);
        }
    }
}

static int count_visible(lv_obj_t *obj, lv_obj_t **first)
{
    int n = 0;
    uint32_t i;
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        int w = (int)lv_area_get_width(&a);
        int h = (int)lv_area_get_height(&a);
        printf("  child[%u] pos=(%d,%d) size=%dx%d hidden=%d\n", (unsigned)i,
               (int)a.x1, (int)a.y1, w, h,
               (int)lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN));
        if (w > 0 && h > 0) {
            n++;
            if (first != NULL && *first == NULL) {
                *first = child;
            }
        }
    }
    return n;
}

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        printf("cannot write %s\n", path);
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * (size_t)h * 3, fp);
    fclose(fp);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

static void ppm_from_snapshot(const lv_draw_buf_t *buf, const char *path)
{
    /* The snapshot is ARGB8888; convert to packed RGB for PPM. */
    int w = (int)buf->header.w;
    int h = (int)buf->header.h;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    if (rgb == NULL) {
        return;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *row = buf->data + (size_t)y * buf->header.stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *px = row + (size_t)x * 4;
            rgb[((size_t)y * w + x) * 3 + 0] = px[2];
            rgb[((size_t)y * w + x) * 3 + 1] = px[1];
            rgb[((size_t)y * w + x) * 3 + 2] = px[0];
        }
    }

    write_ppm(path, rgb, w, h);
    free(rgb);
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "ime_render.ppm";
    const char *mode = (argc > 2) ? argv[2] : "k26";
    /* "-" means "none": shells drop empty arguments, so a placeholder keeps the
     * positional arguments aligned. */
    const char *pinyin = (argc > 3 && strcmp(argv[3], "-") != 0) ? argv[3] : "";
    const char *action = (argc > 4 && strcmp(argv[4], "-") != 0) ? argv[4] : "";

    lv_init();

    lv_display_t *disp = lv_display_create(320, 240);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf, NULL, sizeof(s_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(scr);
    lv_obj_set_width(ta, 300);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "输入文字...");

    printf("creating the IME ...\n");
    lv_obj_t *ime = lv_pinyin_ime_create(scr);
    printf("lv_pinyin_ime_create -> %p\n", (void *)ime);
    printf("fonts: big=%p small=%p (%s)\n", (void *)ime_font_big(), (void *)ime_font_small(),
           ime_font_big() != NULL ? "IME fonts" : "LV_FONT_DEFAULT");
    if (ime == NULL) {
        printf("RESULT: the widget was not created (engine unavailable)\n");
        return 1;
    }

    lv_obj_align(ime, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_pinyin_ime_attach(ime, ta);

    if (strcmp(mode, "k9") == 0) {
        lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K9);
    } else if (strcmp(mode, "p1") == 0) {
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_NUM_SYM);
    } else if (strcmp(mode, "en") == 0) {
        lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_EN);
    }

    /* Pre-fill the text area so the backspace and clear gestures have something
     * to work on. */
    if (strcmp(action, "hold") == 0 || strcmp(action, "clear") == 0) {
        lv_textarea_set_text(ta, "abcdef");
    }

    if (pinyin[0] != '\0') {
        type_text(ime, pinyin);
    }

    if (strcmp(action, "shift") == 0 || strcmp(action, "shift2") == 0) {
        lv_obj_t *shift = find_key(ime, IME_KEY_SHIFT);
        printf("shift key: %p\n", (void *)shift);
        if (shift != NULL) {
            lv_obj_send_event(shift, LV_EVENT_CLICKED, NULL);
            if (strcmp(action, "shift2") == 0) {
                lv_obj_send_event(shift, LV_EVENT_CLICKED, NULL);
            }
        }
        /* One shot / lock: type one letter so a one-shot is consumed. */
        ime_session_push_letter('a');
        ime_session_push_letter('b');
        ime_ui_commit_pending(ime_ctx_get(ime));
        /* Rebuild so the render shows the captions the state resolves to now. */
        ime_ui_rebuild_keyboard(ime_ctx_get(ime));
        ime_ui_refresh_all(ime_ctx_get(ime));
        printf("shift state after typing: %d (0=off 1=once 2=lock), textarea=\"%s\"\n",
               (int)ime_session_shift(), lv_textarea_get_text(ta));
    }

    if (strcmp(action, "hold") == 0) {
        /* Hold the backspace key: the hint pill has to appear. */
        lv_obj_t *bksp = find_key(ime, IME_KEY_BACKSPACE);
        printf("backspace key: %p\n", (void *)bksp);
        lv_obj_send_event(bksp, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(bksp, LV_EVENT_LONG_PRESSED, NULL);
        printf("hint visible: %d, drag needed: %d px\n",
               lv_obj_is_visible(ime_ctx_get(ime)->bksp_hint),
               (int)ime_ctx_get(ime)->bksp_drag_px);
    }

    if (strcmp(action, "clear") == 0) {
        /* Hold, then slide left past the pill: the text area has to be cleared. */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        ime_ui_bksp_hold_begin(ctx, 300);
        ime_ui_bksp_hold_arm(ctx);
        int32_t need = ctx->bksp_drag_px;
        ime_ui_bksp_hold_move(ctx, 300 - (need / 2));
        printf("textarea after half the slide: \"%s\"\n", lv_textarea_get_text(ta));
        ime_ui_bksp_hold_move(ctx, 300 - need - 5);
        printf("textarea after the full slide: \"%s\"\n", lv_textarea_get_text(ta));
        ime_ui_bksp_hold_end(ctx);
    }

    lv_obj_update_layout(scr);

    lv_area_t ime_area;
    lv_obj_get_coords(ime, &ime_area);
    printf("IME coords: (%d,%d)-(%d,%d) size %dx%d, hidden=%d\n", (int)ime_area.x1,
           (int)ime_area.y1, (int)ime_area.x2, (int)ime_area.y2,
           (int)lv_area_get_width(&ime_area), (int)lv_area_get_height(&ime_area),
           (int)lv_obj_has_flag(ime, LV_OBJ_FLAG_HIDDEN));

    printf("IME children:\n");
    lv_obj_t *first_visible = NULL;
    int visible = count_visible(ime, &first_visible);
    printf("visible children: %d\n", visible);

    if (first_visible != NULL) {
        printf("first visible child's children:\n");
        count_visible(first_visible, NULL);
    }

    printf("--- every widget, with its text ---\n");
    dump_labels(scr, 0);

    lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_ARGB8888);
    if (snap == NULL) {
        printf("snapshot failed (is LV_USE_SNAPSHOT on?)\n");
        return 1;
    }
    ppm_from_snapshot(snap, out);
    lv_draw_buf_destroy(snap);

    printf("RESULT: %s\n", visible > 0 ? "objects are laid out" : "NOTHING VISIBLE");
    return visible > 0 ? 0 : 1;
}
