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
 *   cmake -S test_apps/host_ui_render -B build/host_ui_render -G "MinGW Makefiles"
 *   cmake --build build/host_ui_render
 *   ./build/host_ui_render/host_ui_render out.ppm [k26|k9|p1|p1alt|p2|p3|en] [拼音| -] [动作| -] [宽x高] [目标高度]
 *
 * The third argument types lower-case letters through the session (so candidates
 * and the pinyin chip appear); the fourth drives one interaction and *asserts*
 * its result (the exit code is non-zero on failure):
 *
 *   layout   the geometry of the current keyboard / panel: every key inside the
 *            area, no overlaps, even gaps, centered rows, the letters the same
 *            width on every row, and every widget exactly where the layout put
 *            it. This is the multi-resolution check: run it with 240x320,
 *            320x240, 480x320, 800x480 ... and with a target height (the sixth
 *            argument) to exercise the fit path.
 *   shift    English: the caps-lock key is a one-shot, and it must return to off
 *            after the first letter; Chinese: the key must do nothing at all
 *   shift2   English: the second press locks, every letter is upper case
 *   oneshot  English: press shift, tap a letter - the caption goes back to lower
 *            case (run it with the "en" mode)
 *   hold     hold the backspace key: the "左滑清空输入框" hint has to appear
 *   clear    hold, then slide left past the hint: the text area is cleared
 *   chip     the pinyin chip floats above the widget without reserving space
 *   fit      long candidates have to fit the candidate row
 *   press    a pressed candidate / letter / latched shift key has a visible fill
 *
 * 语音页（三个区段 + 圆形麦克风）需要 -DIME_HOST_VOICE=ON 配置出来的构建
 * （见 CMakeLists.txt），mode 用 voice；`layout` 会断言区段首尾相接、圆居中。
 *
 * PPM is not readable by most viewers; tools/ppm2png.py converts it (standard
 * library only):
 *
 *   tools/venv/Scripts/python.exe tools/ppm2png.py out.ppm out.png
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

static int s_failures;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        s_failures++;
    }
}

/**
 * Find a key button: by action, and for IME_KEY_LETTER also by its (always
 * lower-case) label. `letter == '\0'` matches any letter key.
 *
 * ★ 按键现在是 kb_rows 的**直接子对象**（布局引擎给绝对矩形，没有行容器那一层），
 *   所以这里直接遍历它的子对象，每个键的 user_data 就是键表里的那一项。
 */
static lv_obj_t *find_key_in(lv_pinyin_ime_ctx_t *ctx, ime_key_action_t action, char letter)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return NULL;
    }
    uint32_t keys = lv_obj_get_child_count(ctx->kb_rows);
    for (uint32_t k = 0; k < keys; k++) {
        lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, k);
        const ime_key_t *key = lv_obj_get_user_data(btn);
        if (key == NULL || key->action != action) {
            continue;
        }
        if (letter != '\0' && key->label[0] != letter) {
            continue;
        }
        return btn;
    }
    return NULL;
}

/** Find a non-letter key (shift, backspace, ...) by its action. */
static lv_obj_t *find_key(lv_obj_t *ime, ime_key_action_t action)
{
    return find_key_in(ime_ctx_get(ime), action, '\0');
}

/** Find a letter key by its (lower-case) label. */
static lv_obj_t *find_letter_key(lv_obj_t *ime, char letter)
{
    return find_key_in(ime_ctx_get(ime), IME_KEY_LETTER, letter);
}

/** Type a pinyin/ASCII string through the real key dispatch path. Digits drive
 * the 9-key path (e.g. "64" = ni), letters the 26-key one. */
static void type_text(lv_obj_t *ime, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\'') {
            ime_session_push_separator();
            continue;
        }
        if (*p >= '2' && *p <= '9') {
            /* The digit is passed as a character (ime_t9_push_digit wants '2'..'9'). */
            ime_session_t9_push_digit(*p);
            continue;
        }
        ime_session_push_letter(*p);
    }
    /*
     * Lay out before refreshing: the candidate bar's width is what decides how
     * many candidates fit, and a widget that has never been laid out reports 0.
     * (In an application the layout runs every frame, so a key press always sees
     * a valid width - the test types a whole string in one go.)
     */
    lv_obj_update_layout(ime);
    ime_ui_commit_pending(ime_ctx_get(ime));
    ime_ui_refresh_all(ime_ctx_get(ime));
    lv_obj_update_layout(ime);
}

static uint8_t s_buf[800 * 600 * 4];

/** 第五个参数（可选）的显示分辨率，"宽x高"，例如 240x320 / 480x320。 */
static void parse_size(const char *arg, int *w, int *h)
{
    int pw = 0, ph = 0;
    if (arg != NULL && sscanf(arg, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0 &&
        (size_t)pw * (size_t)ph * 4 <= sizeof(s_buf)) {
        *w = pw;
        *h = ph;
    }
}

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
    int scr_w = 320, scr_h = 240;
    parse_size((argc > 5) ? argv[5] : NULL, &scr_w, &scr_h);
    /*
     * 第七个参数（可选）"hidden"：先把键盘建在一个**隐藏**的父容器里，再显示出来。
     *
     * ★ 为什么要专门测这一条：按键用的是从 kb_rows 宽度算出来的绝对坐标，而隐藏的
     *   父容器宽度是 0 —— 建键盘的那一刻根本没有几何可算。控件必须能靠
     *   kb_rows 的 LV_EVENT_SIZE_CHANGED（显示时才算得出宽度）把键盘补出来，
     *   否则"先建键盘、后显示输入框"这种用法会得到一个**空键盘**。
     */
    const bool hidden_parent = (argc > 7) && strcmp(argv[7], "hidden") == 0;

    lv_init();

    lv_display_t *disp = lv_display_create(scr_w, scr_h);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf, NULL, sizeof(s_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(scr);
    lv_obj_set_width(ta, scr_w - 20);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "输入文字...");

    /*
     * Host the keyboard in a content-sized box, the way the applications do
     * (app_momotalk 的 s_ime_box 就是这样) - the pinyin chip draws *above* the
     * widget, so this proves the overflow setup reaches through the ancestors too.
     */
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (hidden_parent) {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }

    printf("creating the IME ...\n");
    lv_obj_t *ime = lv_pinyin_ime_create(box);
    printf("lv_pinyin_ime_create -> %p\n", (void *)ime);
    printf("fonts: big=%p small=%p (%s)\n", (void *)ime_font_big(), (void *)ime_font_small(),
           ime_font_big() != NULL ? "IME fonts" : "LV_FONT_DEFAULT");
    if (ime == NULL) {
        printf("RESULT: the widget was not created (engine unavailable)\n");
        return 1;
    }

    if (hidden_parent) {
        /* 建的时候父容器是隐藏的（宽度 0）：键建不出来，只剩候选栏那几行。 */
        printf("hidden parent: %u objects in the keyboard area before it is shown\n",
               (unsigned)lv_obj_get_child_count(ime_ctx_get(ime)->kb_rows));
        lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(scr);   /* 宽度这一步才算出来 -> SIZE_CHANGED -> 补建键盘 */
        printf("hidden parent: %u objects after showing it\n",
               (unsigned)lv_obj_get_child_count(ime_ctx_get(ime)->kb_rows));
    }

    lv_pinyin_ime_attach(ime, ta);

    if (strcmp(mode, "k9") == 0) {
        lv_pinyin_ime_set_mode(ime, LV_PINYIN_IME_MODE_K9);
    } else if (strcmp(mode, "p1") == 0) {
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_NUM_SYM);
    } else if (strcmp(mode, "p1alt") == 0) {
        /* P1 的"其他符号"批次：面板内切换，走的是同一个 panel + panel_alt */
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_NUM_SYM);
        lv_pinyin_ime_ctx_t *c = ime_ctx_get(ime);
        if (c != NULL) {
            c->panel_alt = true;
            ime_ui_rebuild_keyboard(c);
        }
    } else if (strcmp(mode, "p2") == 0) {
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_NUM);
    } else if (strcmp(mode, "p3") == 0) {
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_ALL_SYM);
    } else if (strcmp(mode, "en") == 0) {
        lv_pinyin_ime_set_lang(ime, LV_PINYIN_IME_LANG_EN);
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    } else if (strcmp(mode, "voice") == 0) {
        /* 语音页：需要 -DIME_HOST_VOICE=ON 才编得进来（见 CMakeLists.txt）。 */
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_VOICE);
#endif
    }

    /*
     * 第六个参数（可选）：目标总高度（px），等价于应用调
     * lv_pinyin_ime_set_fit_height()。0/缺省 = 不按目标高度分摊行高。
     */
    const int fit_h = (argc > 6) ? atoi(argv[6]) : 0;
    if (fit_h > 0) {
        lv_pinyin_ime_set_fit_height(fit_h);
        /* 行高是建键盘时定下的，所以改完要重建一次才能看到效果。 */
        ime_ui_rebuild_keyboard(ime_ctx_get(ime));
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
        const bool en = (strcmp(mode, "en") == 0);
        lv_obj_t *shift = find_key(ime, IME_KEY_SHIFT);
        printf("shift key: %p\n", (void *)shift);
        if (shift != NULL) {
            lv_obj_send_event(shift, LV_EVENT_CLICKED, NULL);
            printf("shift state after 1 press: %d\n", (int)ime_session_shift());
            if (en) {
                check(ime_session_shift() == IME_SHIFT_ONCE,
                      "English: first press is a one-shot shift");
            } else {
                check(ime_session_shift() == IME_SHIFT_OFF,
                      "Chinese: the caps-lock key does nothing");
            }
        }
        if (strcmp(action, "shift2") == 0) {
            /* ★ The click rebuilds the key rows, so the old pointer is gone:
             *   look the key up again instead of reusing it. */
            shift = find_key(ime, IME_KEY_SHIFT);
            if (shift != NULL) {
                lv_obj_send_event(shift, LV_EVENT_CLICKED, NULL);
                printf("shift state after 2 presses: %d\n", (int)ime_session_shift());
                check(ime_session_shift() == IME_SHIFT_LOCK,
                      "second press locks the shift");
            }
        }
        /* Type one letter at a time, draining the commit in between exactly like
         * the real key handler does (the session keeps a single pending commit). */
        ime_session_push_letter('a');
        ime_ui_commit_pending(ime_ctx_get(ime));
        ime_session_push_letter('b');
        ime_ui_commit_pending(ime_ctx_get(ime));
        /* Rebuild so the render shows the captions the state resolves to now. */
        ime_ui_rebuild_keyboard(ime_ctx_get(ime));
        ime_ui_refresh_all(ime_ctx_get(ime));
        printf("shift state after typing: %d (0=off 1=once 2=lock), textarea=\"%s\"\n",
               (int)ime_session_shift(), lv_textarea_get_text(ta));

        if (en) {
            const char *txt = lv_textarea_get_text(ta);
            if (strcmp(action, "shift") == 0) {
                /*
                 * ★ The regression that started this: a one-shot shift has to be
                 *   spent by the first letter AND put the state back to off.
                 *   Leaving it at ONCE made the keyboard look locked for good.
                 */
                check(strcmp(txt, "Ab") == 0, "one-shot: first letter upper, rest lower");
                check(ime_session_shift() == IME_SHIFT_OFF,
                      "one-shot: shift returns to off after the letter");
            } else {
                check(strcmp(txt, "AB") == 0, "caps lock: every letter upper");
                check(ime_session_shift() == IME_SHIFT_LOCK,
                      "caps lock: state stays locked");
            }

            /* The shift key must be latched (highlighted) whenever it changes
             * what a letter produces, and plain otherwise. */
            lv_obj_t *key = find_key(ime, IME_KEY_SHIFT);
            const bool want_latched = (strcmp(action, "shift") != 0);
            check(key != NULL &&
                      lv_obj_has_state(key, LV_STATE_CHECKED) == want_latched,
                  want_latched ? "caps lock highlights the shift key"
                               : "after the one-shot is spent the key is plain again");
        }
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
        check(lv_obj_is_visible(ime_ctx_get(ime)->bksp_hint),
              "holding backspace shows the slide-to-clear hint");
        check(ime_ctx_get(ime)->bksp_drag_px > 0, "the hint sets a slide distance");
    }

    if (strcmp(action, "clear") == 0) {
        /* Hold, then slide left past the pill: the text area has to be cleared. */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        ime_ui_bksp_hold_begin(ctx, 300);
        ime_ui_bksp_hold_arm(ctx);
        int32_t need = ctx->bksp_drag_px;
        ime_ui_bksp_hold_move(ctx, 300 - (need / 2));
        printf("textarea after half the slide: \"%s\"\n", lv_textarea_get_text(ta));
        check(strcmp(lv_textarea_get_text(ta), "abcdef") == 0,
              "half a slide does not clear");
        ime_ui_bksp_hold_move(ctx, 300 - need - 5);
        printf("textarea after the full slide: \"%s\"\n", lv_textarea_get_text(ta));
        check(lv_textarea_get_text(ta)[0] == '\0', "a full slide clears the input box");
        ime_ui_bksp_hold_end(ctx);
        check(!lv_obj_is_visible(ctx->bksp_hint), "the hint goes away with the finger");
    }

    if (strcmp(action, "oneshot") == 0) {
        /*
         * The whole English one-shot path driven through the *UI*, which is where
         * the caption bug lived: press shift (one-shot), then tap a letter.
         * The session spends the shift on that letter, and the key captions have
         * to follow - otherwise the keyboard keeps showing upper case while the
         * next letter typed would be lower case.
         */
        lv_obj_t *shift = find_key(ime, IME_KEY_SHIFT);
        if (shift != NULL) {
            lv_obj_send_event(shift, LV_EVENT_CLICKED, NULL);
        }
        check(ime_session_shift() == IME_SHIFT_ONCE, "shift armed as a one-shot");

        lv_obj_t *a = find_letter_key(ime, 'a');
        check(a != NULL, "found the 'a' key");
        if (a != NULL) {
            lv_obj_send_event(a, LV_EVENT_CLICKED, NULL);
        }

        const char *txt = lv_textarea_get_text(ta);
        printf("textarea=\"%s\" shift=%d\n", txt, (int)ime_session_shift());
        check(strcmp(txt, "A") == 0, "the one-shot letter came out upper case");
        check(ime_session_shift() == IME_SHIFT_OFF, "the one-shot was spent");

        /* ★ The captions must be lower case again. The click rebuilds the key
         *   rows, so every pointer has to be looked up again. */
        lv_obj_t *q = find_letter_key(ime, 'q');
        const char *cap = (q != NULL && lv_obj_get_child_count(q) > 0)
                              ? lv_label_get_text(lv_obj_get_child(q, 0))
                              : NULL;
        printf("'q' caption after the one-shot: \"%s\"\n", cap != NULL ? cap : "(none)");
        check(cap != NULL && strcmp(cap, "q") == 0,
              "key captions went back to lower case");
        shift = find_key(ime, IME_KEY_SHIFT);
        check(shift != NULL && !lv_obj_has_state(shift, LV_STATE_CHECKED),
              "the shift key is no longer highlighted");
    }

    if (strcmp(action, "chip") == 0) {
        /*
         * The pinyin chip hangs above the candidate bar, outside the widget's own
         * box. Three things have to hold:
         *   - it shows up as soon as something is being composed,
         *   - it takes no room at all (no reserved band, no height change),
         *   - it really is drawn above the widget (i.e. the ancestors let it
         *     overflow; without that setup LVGL clips it and it is invisible).
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        lv_obj_update_layout(ime);
        const int h_before = lv_obj_get_height(ime);

        ime_session_push_letter('n');
        ime_ui_commit_pending(ctx);
        ime_ui_refresh_all(ctx);
        lv_obj_update_layout(ime);

        const int h_after = lv_obj_get_height(ime);
        check(!lv_obj_has_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN), "the chip appears while typing");
        check(h_after == h_before, "the chip reserves no space (height unchanged)");
        check(lv_obj_get_y2(ctx->chip) <= lv_obj_get_y(ime),
              "the chip is drawn above the widget");
        check(lv_obj_has_flag(ctx->chip, LV_OBJ_FLAG_FLOATING),
              "the chip is floating (skipped by layout and by content size)");
        check(lv_obj_has_flag(ime, LV_OBJ_FLAG_OVERFLOW_VISIBLE),
              "the widget allows its children to overflow");
    }

    if (strcmp(action, "fit") == 0) {
        /*
         * 长拼音会出"中华人民共和国"这种很宽的候选词。一屏放不下就该少放几个，
         * 而不是把后面的候选顶出可视范围（用户报的"有字在显示范围外"）。
         *
         * lv_obj_get_x() 是**相对父对象**的（lv_obj_pos.c:536），所以这里拿候选
         * 相对候选行的位置，跟行自身的 0..内容宽比。
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        const int row_w = lv_obj_get_content_width(ctx->cand_row);
        const bool have_cands = (ime_session_candidate(0) != NULL);
        size_t shown = 0;
        int outside = 0;
        for (size_t i = 0; i < IME_UI_CAND_MAX; i++) {
            lv_obj_t *b = ctx->cand_btns[i];
            if (b == NULL || lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            shown++;
            const int bx = lv_obj_get_x(b);
            const int bx2 = bx + lv_obj_get_width(b);
            if (bx < 0 || bx2 > row_w) {
                outside++;
                printf("  candidate %u at %d..%d is outside the row 0..%d\n",
                       (unsigned)i, bx, bx2, row_w);
            } else {
                printf("  candidate %u: button %d..%d, label w=%d\n", (unsigned)i, bx, bx2,
                       (int)lv_obj_get_width(lv_obj_get_child(b, 0)));
            }
        }
        printf("  row content width %d, showing %u candidates, page size %u\n",
               row_w, (unsigned)shown, (unsigned)ime_session_page_size());
        if (have_cands) {
            check(shown > 0, "at least one candidate is shown");
            check(outside == 0, "no candidate sticks out of the candidate row");
            check(ime_session_page_size() <= 8, "the page size stays within the maximum");
        } else {
            check(shown == 0, "no candidates while nothing is being composed");
        }
    }

    if (strcmp(action, "press") == 0) {
        /*
         * Pressed feedback. Three cases at once, because they used to differ:
         *   - a candidate (base style has a transparent background)
         *   - a letter key (base style has the opaque key fill)
         *   - the shift key, latched (caps lock) *and* pressed
         * The first one is the regression: the pressed style only set a colour,
         * not bg_opa, so a candidate never changed appearance when tapped.
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        lv_obj_t *cand = NULL;
        for (size_t i = 0; i < IME_UI_CAND_MAX; i++) {
            if (ctx->cand_btns[i] != NULL &&
                !lv_obj_has_flag(ctx->cand_btns[i], LV_OBJ_FLAG_HIDDEN)) {
                cand = ctx->cand_btns[i];
                break;
            }
        }
        if (cand != NULL) {
            lv_obj_add_state(cand, LV_STATE_PRESSED);
        }

        lv_obj_t *letter = NULL;
        for (uint32_t k = 0; k < lv_obj_get_child_count(ctx->kb_rows); k++) {
            lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, k);
            const ime_key_t *key = lv_obj_get_user_data(btn);
            if (key != NULL && key->action == IME_KEY_LETTER && key->label[0] == 'q') {
                letter = btn;
            }
        }
        if (letter != NULL) {
            lv_obj_add_state(letter, LV_STATE_PRESSED);
        }

        lv_obj_t *shift = find_key(ime, IME_KEY_SHIFT);
        if (shift != NULL) {
            lv_obj_add_state(shift, LV_STATE_CHECKED);
            lv_obj_add_state(shift, LV_STATE_PRESSED);
        }

        /* The resolved style of the pressed state decides whether anything is
         * actually drawn; a colour with a transparent background is invisible. */
        const bool cn = (strcmp(mode, "en") != 0);
        check(cand == NULL || lv_obj_get_style_bg_opa(cand, LV_PART_MAIN) > LV_OPA_MIN,
              "a pressed candidate has an opaque background");
        if (cn) {
            /* English letters commit straight away, so there is no candidate row
             * to press there - only Chinese input produces candidates. */
            check(cand != NULL, "there was a visible candidate to press");
        }
        check(letter != NULL &&
                  lv_obj_get_style_bg_opa(letter, LV_PART_MAIN) > LV_OPA_MIN,
              "a pressed letter key has an opaque background");
    }

    if (strcmp(action, "layout") == 0 && strcmp(mode, "voice") != 0) {
        /*
         * 布局自检 + 多分辨率验收（对应《输入法布局优化指南》§11 的验收表）。
         *
         * 断言的是**算出来的几何**和**控件真实的坐标**两件事：前者由 ime_layout 保证，
         * 后者由渲染层保证。两者对不上（算了不用、或者用了旧的）是这类重构最容易
         * 留下的问题，而它在屏幕上只表现为"某个键偏了几像素"，肉眼很难定位。
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        lv_obj_update_layout(scr);

        const ime_layout_metrics_t *m = &ctx->kb_metrics;
        const ime_layout_report_t *rep = &ctx->kb_report;
        printf("layout: area %dx%d rows=%d row_h=%d unit=%d.%03dpx grid_x=%d grid_w=%d "
               "gaps %d/%d used_h=%d compact=%d tight=%d\n",
               (int)m->width, (int)m->height, (int)m->row_count, (int)m->row_height,
               (int)(m->unit_milli / 1000), (int)(m->unit_milli % 1000), (int)m->x_offset,
               (int)m->content_width, (int)m->key_gap, (int)m->row_gap, (int)m->used_height,
               (int)m->compact, (int)m->tight);
        printf("layout check: keys=%u oob=%u overlap=%u gap=%u align=%u w=%u h=%u\n",
               (unsigned)rep->keys, (unsigned)rep->out_of_bounds, (unsigned)rep->overlaps,
               (unsigned)rep->bad_gaps, (unsigned)rep->misaligned,
               (unsigned)rep->width_violations, (unsigned)rep->height_violations);

        check(rep->keys > 0, "there are keys to check");        check(rep->out_of_bounds == 0, "no key sticks out of the keyboard area");
        check(rep->overlaps == 0, "no two keys overlap");
        check(rep->bad_gaps == 0, "the gap between two keys is the theme's gap");
        check(rep->misaligned == 0, "every centered row is centered");
        check(rep->width_violations == 0, "no key is below the minimum touch width");
        check(rep->height_violations == 0, "the row height is within its limits");
        check(!m->tight, "the layout fits without shrinking below the design minimum");

        /* 每个键的矩形 vs 控件的真实坐标 */
        const ime_keyboard_t *kb = ime_ui_active_keyboard(ctx);
        ime_key_rect_t rects[IME_UI_KEY_MAX];
        const uint32_t keys = lv_obj_get_child_count(ctx->kb_rows);

        /*
         * 键面的实际填充：普通键用 IME_COLOR_KEY（白），功能键用 IME_COLOR_SPECIAL
         * （浅灰）。bg_opa = 0 的键在屏幕上看就是"只有文字和描边、没有键面"——
         * lv_obj_remove_style_all() 摘掉了主题那层背景，样式只携带自己定义过的属性，
         * 所以每个键的样式都必须**自己**写 bg_opa（s_key_special 曾经漏了这行，
         * 于是 ⌫ / ⏎ / 分词 / 整个底部功能行全是透空的）。
         */
        {
            int transparent = 0;
            for (uint32_t i = 0; i < keys; i++) {
                lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, i);
                if (lv_obj_get_style_bg_opa(btn, LV_PART_MAIN) <= LV_OPA_MIN &&
                    !lv_obj_has_state(btn, LV_STATE_DISABLED)) {
                    transparent++;
                }
            }
            printf("  keys without a visible fill: %d of %u\n", transparent, (unsigned)keys);
            check(transparent == 0, "every enabled key has an opaque fill");
        }
        if (ime_ui_calculate_layout(ctx, kb, rects, IME_UI_KEY_MAX)) {
            int mismatch = 0;
            for (uint32_t i = 0; i < keys && i < IME_UI_KEY_MAX; i++) {
                lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, i);
                if (lv_obj_get_x(btn) != rects[i].x || lv_obj_get_y(btn) != rects[i].y ||
                    lv_obj_get_width(btn) != rects[i].width ||
                    lv_obj_get_height(btn) != rects[i].height) {
                    mismatch++;
                    printf("  key %u: widget (%d,%d) %dx%d vs layout (%d,%d) %dx%d\n",
                           (unsigned)i, (int)lv_obj_get_x(btn), (int)lv_obj_get_y(btn),
                           (int)lv_obj_get_width(btn), (int)lv_obj_get_height(btn),
                           (int)rects[i].x, (int)rects[i].y, (int)rects[i].width,
                           (int)rects[i].height);
                }
            }
            check(mismatch == 0, "every key sits exactly where the layout put it");
        }

        /*
         * 26 键：**同一列**的字母宽度必须完全相同（标准键宽 + 边缘吸附的直接证据）。
         * q（R1，在 1.5u 的 符 之后）与 a（R2，在 1.5u 的 ⇧ 之后）处在同一个单位位置；
         * w（R1，2.4u 处）与 z（R3，1.5u + 0.9u 之后 = 2.4u 处）同理。
         * 旧实现每行各自分摊宽度，这些值是三套不同的数字。
         */
        if (strcmp(mode, "k26") == 0 || strcmp(mode, "en") == 0) {
            lv_obj_t *q = find_letter_key(ime, 'q');
            lv_obj_t *a = find_letter_key(ime, 'a');
            lv_obj_t *w = find_letter_key(ime, 'w');
            lv_obj_t *z = find_letter_key(ime, 'z');
            const int wq = (q != NULL) ? lv_obj_get_width(q) : -1;
            const int wa = (a != NULL) ? lv_obj_get_width(a) : -2;
            const int ww = (w != NULL) ? lv_obj_get_width(w) : -3;
            const int wz = (z != NULL) ? lv_obj_get_width(z) : -4;
            printf("  letter widths: q=%d a=%d (both at 1.5u), w=%d z=%d (both at 2.4u)\n", wq, wa,
                   ww, wz);
            check(wq > 0 && wq == wa, "q and a are the same width (same column)");
            check(ww > 0 && ww == wz, "w and z are the same width (same column)");
        }

        /* 每一行的左右边界与行高（配合多分辨率一起看） */
        int last_y = -1;        for (uint32_t i = 0; i < keys; i++) {
            lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, i);
            const int y = lv_obj_get_y(btn);
            if (y != last_y) {
                last_y = y;
                printf("  row at y=%d h=%d: ", y, (int)lv_obj_get_height(btn));
            }
            printf("[%s x=%d w=%d] ", lv_obj_get_user_data(btn) != NULL
                                         ? ((const ime_key_t *)lv_obj_get_user_data(btn))->label
                                         : "?",
                   (int)lv_obj_get_x(btn), (int)lv_obj_get_width(btn));
            lv_obj_t *next = (i + 1 < keys) ? lv_obj_get_child(ctx->kb_rows, i + 1) : NULL;
            if (next == NULL || lv_obj_get_y(next) != y) {
                printf("\n");
            }
        }
    }

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    if (strcmp(action, "voiceback") == 0) {
        /*
         * 去语音页再回来（用户报的"点了语音输入再返回 26 键，空格下面留白一部分"）。
         *
         * 语音页会把 kb_rows 的高度设成**确定值**（那一页只有三区段，要靠高度分摊），
         * 返回普通键盘时如果不还原成 LV_SIZE_CONTENT，键盘下面就会留出语音页多出来的
         * 那几十个像素 —— 一片什么都没有的底板。
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_VOICE);
        lv_obj_update_layout(scr);
        const int voice_h = lv_obj_get_height(ctx->kb_rows);
        const uint32_t voice_children = lv_obj_get_child_count(ctx->kb_rows);

        lv_pinyin_ime_set_panel(ime, LV_PINYIN_IME_PANEL_MAIN);
        lv_obj_update_layout(scr);
        const int back_h = lv_obj_get_height(ctx->kb_rows);
        const uint32_t back_children = lv_obj_get_child_count(ctx->kb_rows);

        /* 键盘内容高度 = 最后一个键的底边（rects 从 y=0 排下来） */
        lv_obj_t *last = (back_children > 0) ? lv_obj_get_child(ctx->kb_rows, back_children - 1)
                                             : NULL;
        const int content_h = (last != NULL) ? lv_obj_get_y(last) + lv_obj_get_height(last) : -1;

        printf("voiceback: voice page %u objects / %d px -> keyboard %u objects / %d px "
               "(content %d px)\n",
               (unsigned)voice_children, voice_h, (unsigned)back_children, back_h, content_h);
        check(voice_children == 3, "the voice page has its three zones");
        check(back_h == content_h,
              "the keyboard area is exactly as tall as its rows (no blank strip below)");
        check(back_h < voice_h, "and it is shorter than the voice page was");
        check(ctx->voice_page_status == NULL && ctx->voice_page_mic == NULL,
              "the voice page pointers are cleared (no dangling objects)");
    }
#endif

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    if (strcmp(action, "layout") == 0 && strcmp(mode, "voice") == 0) {
        /*
         * 语音页的验收（指南 §11 那一行"圆形麦克风保持居中，底部操作区稳定"）：
         * 三个区段必须首尾相接、互不重叠，圆的直径由可用宽高约束、并真正居中。
         */
        lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
        lv_obj_update_layout(scr);

        const uint32_t zones = lv_obj_get_child_count(ctx->kb_rows);
        const int kb_h = lv_obj_get_height(ctx->kb_rows);
        const int kb_w = lv_obj_get_width(ctx->kb_rows);
        printf("voice page: %ux%d, %u zones\n", kb_w, kb_h, (unsigned)zones);
        check(zones == 3, "the voice page has three zones");

        int prev_bottom = -1;
        for (uint32_t i = 0; i < zones; i++) {
            lv_obj_t *z = lv_obj_get_child(ctx->kb_rows, i);
            const int zx = lv_obj_get_x(z), zy = lv_obj_get_y(z);
            const int zw = lv_obj_get_width(z), zh = lv_obj_get_height(z);
            printf("  zone %u: (%d,%d) %dx%d, %u children\n", (unsigned)i, zx, zy, zw, zh,
                   (unsigned)lv_obj_get_child_count(z));
            check(zx >= 0 && zy >= 0 && zx + zw <= kb_w && zy + zh <= kb_h,
                  "the zone is inside the keyboard area");
            check(zh > 0 && zw > 0, "the zone has a real size");
            if (i > 0) {
                check(zy >= prev_bottom, "the zones do not overlap");
            }
            prev_bottom = zy + zh;
        }
        check(prev_bottom == kb_h, "the zones fill the keyboard area exactly");

        /* 圆形麦克风：正方形（= 圆）、在自己的区段里居中、不越界。 */
        lv_obj_t *mic = ctx->voice_page_mic;
        check(mic != NULL, "the mic button exists");
        if (mic != NULL) {
            lv_obj_t *zone = lv_obj_get_parent(mic);
            lv_area_t za, ma;
            lv_obj_get_coords(zone, &za);   /* 绝对坐标：区段与圆各在不同的父对象里 */
            lv_obj_get_coords(mic, &ma);
            const int d = lv_area_get_width(&ma);
            const int dx = ((za.x1 + za.x2) / 2) - ((ma.x1 + ma.x2) / 2);
            const int dy = ((za.y1 + za.y2) / 2) - ((ma.y1 + ma.y2) / 2);
            printf("  mic: d=%d, offset from the zone centre (%d,%d)\n", d, dx, dy);
            check(lv_area_get_width(&ma) == lv_area_get_height(&ma),
                  "the mic is a circle (square button + full radius)");
            check(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1, "the mic is centered in its zone");
            check(d >= 24, "the mic is big enough to hit");
            check(d <= lv_area_get_width(&za) && d <= lv_area_get_height(&za),
                  "the mic fits inside its zone");
        }
    }
#endif

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

    printf("RESULT: %s (%d check failure(s))\n",
           (visible > 0 && s_failures == 0) ? "objects are laid out" : "FAILURES",
           s_failures);
    return (visible > 0 && s_failures == 0) ? 0 : 1;
}
