/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The key tables, transcribed from docs/keymap.md, and the code that turns a
 * table into LVGL buttons.
 *
 * Widths are in tenths of a unit: the 26-key keyboard is 12u wide and the
 * symbol panels are 10u wide, so every row has to add up to 120 or 100.
 */

#include "lv_pinyin_ime_internal.h"

#include <ctype.h>
#include <string.h>

#include "core/ime_log.h"
#include "data/ime_font.h"

#define K(label, action) {label, NULL, 10, action}
#define KW(label, action, w) {label, NULL, w, action}
#define KT(label, text, action) {label, text, 10, action}
#define KWT(label, text, action, w) {label, text, w, action}

/*
 * 26 key.
 *
 * The letters are 0.9u and the outer keys (符 / ⇧ / 分词 on the left, ⌫ / ⏎ on the
 * right) are 1.5u. Making the outer keys wider than a letter while keeping the row
 * at 12 units is what removes the blank margin at both ends: at 1u each the letter
 * rows came to 279 px of keys inside a 316 px row, and the 37 px left over showed
 * up as dead space on the left and right.
 */

/* R1: 符 q w e r t y u i o p ⌫ */
static const ime_key_t s_k26_r1[] = {
    KWT("符", NULL, IME_KEY_PANEL, 15),
    KW("q", IME_KEY_LETTER, 9), KW("w", IME_KEY_LETTER, 9), KW("e", IME_KEY_LETTER, 9),
    KW("r", IME_KEY_LETTER, 9), KW("t", IME_KEY_LETTER, 9), KW("y", IME_KEY_LETTER, 9),
    KW("u", IME_KEY_LETTER, 9), KW("i", IME_KEY_LETTER, 9), KW("o", IME_KEY_LETTER, 9),
    KW("p", IME_KEY_LETTER, 9),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 15),
};

/* R2: ⇧ a s d f g h j k l ⏎ */
static const ime_key_t s_k26_r2[] = {
    KWT(LV_SYMBOL_UP, NULL, IME_KEY_SHIFT, 15),
    KW("a", IME_KEY_LETTER, 9), KW("s", IME_KEY_LETTER, 9), KW("d", IME_KEY_LETTER, 9),
    KW("f", IME_KEY_LETTER, 9), KW("g", IME_KEY_LETTER, 9), KW("h", IME_KEY_LETTER, 9),
    KW("j", IME_KEY_LETTER, 9), KW("k", IME_KEY_LETTER, 9), KW("l", IME_KEY_LETTER, 9),
    KWT(LV_SYMBOL_NEW_LINE, NULL, IME_KEY_ENTER, 15),
};

/* R3: 分词 - z x c v b n m . , : */
static const ime_key_t s_k26_r3[] = {
    KWT("分词", NULL, IME_KEY_SEPARATOR, 15),
    KWT("-", NULL, IME_KEY_FULLWIDTH, 9),
    KW("z", IME_KEY_LETTER, 9), KW("x", IME_KEY_LETTER, 9), KW("c", IME_KEY_LETTER, 9),
    KW("v", IME_KEY_LETTER, 9), KW("b", IME_KEY_LETTER, 9), KW("n", IME_KEY_LETTER, 9),
    KW("m", IME_KEY_LETTER, 9),
    KWT(".", NULL, IME_KEY_FULLWIDTH, 9), KWT(",", NULL, IME_KEY_FULLWIDTH, 9),
    KWT(":", NULL, IME_KEY_FULLWIDTH, 9),
};

/*
 * R4: ?123(2u) ⌨(2u) 空格(4u) 中/英(2u) ✓(2u)
 *
 * The space bar sits between the keyboard switch and the language key, which
 * puts it under the thumbs and leaves the two mode keys in the outer corners -
 * the arrangement the reference layout uses. It is 4u wide so it lines up with
 * the letter columns above it.
 */
static const ime_key_t s_k26_r4[] = {
    KWT("?123", NULL, IME_KEY_PANEL, 20),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 20),
    KWT("空格", NULL, IME_KEY_SPACE, 40),
    KWT("中", NULL, IME_KEY_LANG, 20),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 20),
};

/* English mode: same geometry, the separator key is inert. */
static const ime_key_t s_k26_en_r3[] = {
    KWT("分词", NULL, IME_KEY_NONE, 15),
    KWT("-", NULL, IME_KEY_FULLWIDTH, 9),
    KW("z", IME_KEY_LETTER, 9), KW("x", IME_KEY_LETTER, 9), KW("c", IME_KEY_LETTER, 9),
    KW("v", IME_KEY_LETTER, 9), KW("b", IME_KEY_LETTER, 9), KW("n", IME_KEY_LETTER, 9),
    KW("m", IME_KEY_LETTER, 9),
    KWT(".", NULL, IME_KEY_FULLWIDTH, 9), KWT(",", NULL, IME_KEY_FULLWIDTH, 9),
    KWT(":", NULL, IME_KEY_FULLWIDTH, 9),
};

/* ------------------------------------------------------------------- 9 key */

/*
 * 9 key. Every key is 2 units wide and the rows add up to 120 tenths of a unit,
 * the same as the 26-key rows, so both keyboards share one column grid and the
 * four rows line up vertically.
 */
static const ime_key_t s_k9_r1[] = {
    KWT("123", NULL, IME_KEY_PANEL, 24),
    KWT(",。?!", NULL, IME_KEY_PANEL, 24),
    KWT("ABC", NULL, IME_KEY_DIGIT, 24), KWT("DEF", NULL, IME_KEY_DIGIT, 24),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 24),
};

/* R2: 。 4\nGHI 5\nJKL 6\nMNO 重输 */
static const ime_key_t s_k9_r2[] = {
    KWT("中", NULL, IME_KEY_LANG, 24),
    KWT("GHI", NULL, IME_KEY_DIGIT, 24), KWT("JKL", NULL, IME_KEY_DIGIT, 24),
    KWT("MNO", NULL, IME_KEY_DIGIT, 24),
    KWT("分隔", NULL, IME_KEY_SEPARATOR, 24),
};

/* R3: ? 7\nPQRS 8\nTUV 9\nWXYZ 0 */
static const ime_key_t s_k9_r3[] = {
    KWT("拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT("PQRS", NULL, IME_KEY_DIGIT, 24), KWT("TUV", NULL, IME_KEY_DIGIT, 24),
    KWT("WXYZ", NULL, IME_KEY_DIGIT, 24),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 24),
};

/* R4: 符/123 空格 中/英 ↵ */
static const ime_key_t s_k9_r4[] = {
    KWT("选拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 24),
    KWT("空格", NULL, IME_KEY_SPACE, 72),
};

/* ------------------------------------------------------- P1 digits + symbols */

/*
 * The panel behind "符/123". Row 1 is the digits, row 2 the punctuation the IME
 * emits most, row 3 the arithmetic signs, and the last row navigates between the
 * panels.
 *
 * There used to be two separate panels here (P1 "digits + common symbols" and P2
 * "digits only") reached by two different keys, but both opened the same layout,
 * so they are one panel and one key now.
 */
static const ime_key_t s_p1_r1[] = {
    KT("1", "1", IME_KEY_TEXT), KT("2", "2", IME_KEY_TEXT), KT("3", "3", IME_KEY_TEXT),
    KT("4", "4", IME_KEY_TEXT), KT("5", "5", IME_KEY_TEXT), KT("6", "6", IME_KEY_TEXT),
    KT("7", "7", IME_KEY_TEXT), KT("8", "8", IME_KEY_TEXT), KT("9", "9", IME_KEY_TEXT),
    KT("0", "0", IME_KEY_TEXT),
};

static const ime_key_t s_p1_r2[] = {
    KT("@", "@", IME_KEY_TEXT), KT("#", "#", IME_KEY_TEXT),
    KT("￥", "￥", IME_KEY_TEXT), KT("%", "%", IME_KEY_TEXT),
    KT("&", "&", IME_KEY_TEXT), KT("*", "*", IME_KEY_TEXT),
    KT("-", "-", IME_KEY_TEXT), KT("+", "+", IME_KEY_TEXT),
    KT("(", "(", IME_KEY_TEXT), KT(")", ")", IME_KEY_TEXT),
};

static const ime_key_t s_p1_r3[] = {
    KWT("更多", "all", IME_KEY_PANEL, 40),
    KWT("其他符号", "alt", IME_KEY_PANEL, 40),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 20),
};

static const ime_key_t s_p1_r4[] = {
    KWT("ABC", "main", IME_KEY_PANEL, 20),
    KWT("123", "num", IME_KEY_PANEL, 20),
    KWT("空格", NULL, IME_KEY_SPACE, 40),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 20),
};

/* The "其他符号" batch inside P1. */
static const ime_key_t s_p1a_r1[] = {
    KT("~", "~", IME_KEY_TEXT), KT("!", "!", IME_KEY_TEXT), KT("?", "?", IME_KEY_TEXT),
    KT("/", "/", IME_KEY_TEXT), KT("\\", "\\", IME_KEY_TEXT), KT("|", "|", IME_KEY_TEXT),
    KT("[", "[", IME_KEY_TEXT), KT("]", "]", IME_KEY_TEXT), KT("{", "{", IME_KEY_TEXT),
    KT("}", "}", IME_KEY_TEXT),
};

static const ime_key_t s_p1a_r2[] = {
    KT("<", "<", IME_KEY_TEXT), KT(">", ">", IME_KEY_TEXT), KT("=", "=", IME_KEY_TEXT),
    KT("_", "_", IME_KEY_TEXT), KT("\"", "\"", IME_KEY_TEXT), KT("'", "'", IME_KEY_TEXT),
    KT(";", ";", IME_KEY_TEXT), KT("^", "^", IME_KEY_TEXT), KT("`", "`", IME_KEY_TEXT),
    KT("$", "$", IME_KEY_TEXT),
};


/* ----------------------------------------------------------- P2 digits only */
static const ime_key_t s_p2_r1[] = {
    KT("1", "1", IME_KEY_TEXT), KT("2", "2", IME_KEY_TEXT), KT("3", "3", IME_KEY_TEXT),
};
static const ime_key_t s_p2_r2[] = {
    KT("4", "4", IME_KEY_TEXT), KT("5", "5", IME_KEY_TEXT), KT("6", "6", IME_KEY_TEXT),
};
static const ime_key_t s_p2_r3[] = {
    KT("7", "7", IME_KEY_TEXT), KT("8", "8", IME_KEY_TEXT), KT("9", "9", IME_KEY_TEXT),
};
static const ime_key_t s_p2_r4[] = {
    KWT("ABC", "main", IME_KEY_PANEL, 10),
    KT("0", "0", IME_KEY_TEXT),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 10),
};



/* ----------------------------------------------------------- P3 all symbols */

static const ime_key_t s_p3_r1[] = {
    KT("~", "~", IME_KEY_TEXT), KT("!", "!", IME_KEY_TEXT), KT("?", "?", IME_KEY_TEXT),
    KT("/", "/", IME_KEY_TEXT), KT("\\", "\\", IME_KEY_TEXT), KT("|", "|", IME_KEY_TEXT),
    KT("[", "[", IME_KEY_TEXT), KT("]", "]", IME_KEY_TEXT), KT("{", "{", IME_KEY_TEXT),
    KT("}", "}", IME_KEY_TEXT),
};

static const ime_key_t s_p3_r2[] = {
    KT("…", "…", IME_KEY_TEXT), KT("—", "—", IME_KEY_TEXT), KT("、", "、", IME_KEY_TEXT),
    KT("。", "。", IME_KEY_TEXT), KT("，", "，", IME_KEY_TEXT), KT("；", "；", IME_KEY_TEXT),
    KT("：", "：", IME_KEY_TEXT), KT("？", "？", IME_KEY_TEXT), KT("！", "！", IME_KEY_TEXT),
    KT("《", "《", IME_KEY_TEXT),
};

static const ime_key_t s_p3_r3[] = {
    KT("±", "±", IME_KEY_TEXT), KT("×", "×", IME_KEY_TEXT), KT("÷", "÷", IME_KEY_TEXT),
    KT("°", "°", IME_KEY_TEXT), KT("′", "′", IME_KEY_TEXT), KT("″", "″", IME_KEY_TEXT),
    KT("Ω", "Ω", IME_KEY_TEXT), KT("μ", "μ", IME_KEY_TEXT), KT("§", "§", IME_KEY_TEXT),
    KT("¥", "¥", IME_KEY_TEXT),
};

static const ime_key_t s_p3_r4[] = {
    KT("←", "←", IME_KEY_TEXT), KT("→", "→", IME_KEY_TEXT), KT("↑", "↑", IME_KEY_TEXT),
    KT("↓", "↓", IME_KEY_TEXT), KT("⇒", "⇒", IME_KEY_TEXT), KT("⇔", "⇔", IME_KEY_TEXT),
    KT("≈", "≈", IME_KEY_TEXT), KT("≠", "≠", IME_KEY_TEXT), KT("≤", "≤", IME_KEY_TEXT),
    KT("≥", "≥", IME_KEY_TEXT),
};

static const ime_key_t s_p3_r5[] = {
    KWT("ABC", "main", IME_KEY_PANEL, 80),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 20),
};

/* ------------------------------------------------------------------ tables */

#define ROW(keys) {keys, sizeof(keys) / sizeof(keys[0])}

#define ROWS(rows_name, kb_name, ...)                                 \
    static const ime_row_t rows_name[] = {__VA_ARGS__};               \
    static const ime_keyboard_t kb_name = {                           \
        rows_name, sizeof(rows_name) / sizeof(rows_name[0])}

ROWS(s_k26_cn_rows, s_k26_cn_kb, ROW(s_k26_r1), ROW(s_k26_r2), ROW(s_k26_r3), ROW(s_k26_r4));
ROWS(s_k26_en_rows, s_k26_en_kb, ROW(s_k26_r1), ROW(s_k26_r2), ROW(s_k26_en_r3), ROW(s_k26_r4));
ROWS(s_k9_rows, s_k9_kb, ROW(s_k9_r1), ROW(s_k9_r2), ROW(s_k9_r3), ROW(s_k9_r4));
ROWS(s_p1_rows, s_p1_kb, ROW(s_p1_r1), ROW(s_p1_r2), ROW(s_p1_r3), ROW(s_p1_r4));
ROWS(s_p1_alt_rows, s_p1_alt_kb, ROW(s_p1a_r1), ROW(s_p1a_r2), ROW(s_p1_r3), ROW(s_p1_r4));
ROWS(s_p2_rows, s_p2_kb, ROW(s_p2_r1), ROW(s_p2_r2), ROW(s_p2_r3), ROW(s_p2_r4));
ROWS(s_p3_rows, s_p3_kb, ROW(s_p3_r1), ROW(s_p3_r2), ROW(s_p3_r3), ROW(s_p3_r4), ROW(s_p3_r5));

static const ime_keyboard_t *const s_main_kb[IME_MODE_LAST][IME_LANG_LAST] = {
    {&s_k26_cn_kb, &s_k26_en_kb},
    {&s_k9_kb, &s_k9_kb},
};

static const ime_keyboard_t *const s_panel_kb[LV_PINYIN_IME_PANEL_LAST] = {
    NULL, /* P0 is selected by mode / language */
    &s_p1_kb,
    &s_p2_kb,
    &s_p3_kb,
};

const ime_keyboard_t *ime_ui_active_keyboard(const lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx->panel == LV_PINYIN_IME_PANEL_MAIN) {
        ime_mode_t mode = (ctx->mode < IME_MODE_LAST) ? ctx->mode : IME_MODE_K26;
        ime_lang_t lang = (ctx->lang < IME_LANG_LAST) ? ctx->lang : IME_LANG_CN;
        return s_main_kb[mode][lang];
    }
    if (ctx->panel == LV_PINYIN_IME_PANEL_NUM_SYM && ctx->panel_alt) {
        return &s_p1_alt_kb;
    }
    if (ctx->panel < LV_PINYIN_IME_PANEL_LAST) {
        return s_panel_kb[ctx->panel];
    }
    return &s_k26_cn_kb;
}

/* ------------------------------------------------------------------- build */

/** Finger position of the active pointer, or 0 when there is none. */
static int32_t indev_x(void)
{
    const lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return 0;
    }
    lv_point_t p = {0, 0};
    lv_indev_get_point(indev, &p);
    return p.x;
}

static void key_click_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const ime_key_t *key = lv_obj_get_user_data(btn);
    if (ctx == NULL || key == NULL) {
        return;
    }
    /*
     * ★ LVGL sends CLICKED unconditionally on release, even after a long press
     *   (indev_proc_release only skips the *short* click). A hold on the
     *   backspace key has already deleted characters, so that trailing CLICKED
     *   has to be swallowed or the release eats one more.
     */
    if (key->action == IME_KEY_BACKSPACE && ime_ui_bksp_take_consumed(ctx)) {
        return;
    }
    ime_ui_handle_key(ctx, key);
}

static void key_long_press_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const ime_key_t *key = lv_obj_get_user_data(btn);
    if (ctx == NULL || key == NULL) {
        return;
    }

    switch (key->action) {
    case IME_KEY_SHIFT:
        /* Long press switches Chinese / English. */
        ime_ui_set_lang(ctx, (ctx->lang == IME_LANG_CN) ? IME_LANG_EN : IME_LANG_CN);
        ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
        ctx->panel_alt = false;
        ime_ui_refresh_all(ctx);
        break;
    case IME_KEY_BACKSPACE:
        /*
         * Held down: LVGL repeats this every long_press_repeat_time (100 ms), so
         * the whole path of the click has to run here too - deleting only the
         * composition buffer meant a hold did nothing at all once the buffer was
         * empty, and the user only saw the single character the release-time
         * CLICKED removed.
         */
        ctx->bksp_consumed = true;
        ime_ui_backspace(ctx);
        ime_ui_commit_pending(ctx);
        ime_ui_refresh_all(ctx);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------- backspace: hold + gesture */

static void bksp_pressed_cb(lv_event_t *e)
{
    ime_ui_bksp_hold_begin(lv_event_get_user_data(e), indev_x());
}

static void bksp_long_cb(lv_event_t *e)
{
    ime_ui_bksp_hold_arm(lv_event_get_user_data(e));
}

static void bksp_pressing_cb(lv_event_t *e)
{
    ime_ui_bksp_hold_move(lv_event_get_user_data(e), indev_x());
}

static void bksp_hold_end_cb(lv_event_t *e)
{
    ime_ui_bksp_hold_end(lv_event_get_user_data(e));
}

static bool key_is_special(const ime_key_t *key)
{
    switch (key->action) {
    case IME_KEY_LETTER:
    case IME_KEY_DIGIT:
    case IME_KEY_TEXT:
        return false;
    default:
        return true;
    }
}

/*
 * Captions that depend on the current state rather than the key table.
 *
 * The language key shows only the *active* language, one character, the way the
 * reference layout does (中 for Chinese, 英 for English) instead of listing both.
 *
 * Letters exist once in the tables, in lower case, and the caption is cased
 * here:
 *
 *   Chinese  always upper case - a pinyin keyboard is drawn that way, and the
 *            shift key has no effect on it at all (see ime_session_push_letter)
 *   English  upper case while shift is one-shot or locked, lower case otherwise
 *
 * The buffer returned for the upper-case form is a single static because
 * lv_label_set_text() copies the string before the next key is built.
 */
static char s_letter_upper[2];

static const char *key_label(const lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key)
{
    switch (key->action) {
    case IME_KEY_SHIFT:
        /* One symbol for every state; the pressed/locked state is shown by the
         * key style, not by the caption. */
        return LV_SYMBOL_UP;

    case IME_KEY_LANG:
        return (ctx->lang == IME_LANG_CN) ? "中" : "英";

    case IME_KEY_LETTER:
        if (ctx->lang == IME_LANG_CN || ime_session_shift() != IME_SHIFT_OFF) {
            s_letter_upper[0] = (char)toupper((unsigned char)key->label[0]);
            s_letter_upper[1] = '\0';
            return s_letter_upper;
        }
        return key->label;

    default:
        return key->label;
    }
}

/*
 * Which font a key caption needs.
 *
 * Special keys are captioned with LVGL's own symbols (LV_SYMBOL_BACKSPACE and
 * friends). Those live in the FontAwesome private use area and come from the
 * built-in font, which the subsetted CJK font does not carry, so those labels
 * must use LV_FONT_DEFAULT. Everything else - letters and the CJK captions -
 * uses the IME font. This mirrors what LVGL's own keyboard does.
 */
static const lv_font_t *key_caption_font(const ime_key_t *key)
{
    switch (key->action) {
    case IME_KEY_BACKSPACE:
    case IME_KEY_ENTER:
    case IME_KEY_SHIFT:
    case IME_KEY_CONFIRM:
    case IME_KEY_SWITCH_KB:
        return LV_FONT_DEFAULT;
    default:
        return ime_font_small();
    }
}

/*
 * Key geometry.
 *
 * Every key is sized by flexbox rather than in pixels: a key gets
 * `grow = width_u`, so a 1u key is half a 2u key and so on, and the row divides
 * its width between them exactly. That is what makes the keys fill the row edge
 * to edge (no leftover margin on the left or right) and keeps the vertical
 * columns aligned between rows - sizing each key in pixels and hoping the pieces
 * add up to the row width left a different gap after every key.
 *
 * `IME_UI_KEY_GAP` is the space between keys, applied as the row's column padding.
 */

/** Flex grow weight of a key. */
static int key_grow(const ime_key_t *key)
{
    return (key->width_u > 0) ? key->width_u : 10;
}

void ime_ui_apply_keyboard(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb)
{
    lv_obj_clean(ctx->kb_rows);

    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];

        lv_obj_t *row_obj = lv_obj_create(ctx->kb_rows);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_width(row_obj, LV_PCT(100));
        lv_obj_set_height(row_obj, IME_UI_ROW_HEIGHT_PX);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        /* No space between (grow does the dividing) and no wrapping. */
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row_obj, IME_UI_KEY_GAP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row_obj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);

        for (size_t k = 0; k < row->count; k++) {
            const ime_key_t *key = &row->keys[k];

            lv_obj_t *btn = lv_button_create(row_obj);
            /*
             * Style first: ime_style_apply_key() starts with
             * lv_obj_remove_style_all(), which also clears local properties
             * already set on the object.
             */
            ime_style_apply_key(btn, key_is_special(key));
            lv_obj_set_height(btn, LV_PCT(100));
            /* Width comes from the flex weight, so the row fills edge to edge. */
            lv_obj_set_flex_grow(btn, key_grow(key));
            lv_obj_set_user_data(btn, (void *)key);

            lv_obj_t *label = lv_label_create(btn);
            lv_label_set_text(label, key_label(ctx, key));
            lv_label_set_recolor(label, true);
            ime_style_apply_font(label, key_caption_font(key));
            lv_obj_center(label);

            if (key->action == IME_KEY_NONE) {
                lv_obj_add_state(btn, LV_STATE_DISABLED);
                continue;
            }

            lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(btn, key_long_press_cb, LV_EVENT_LONG_PRESSED_REPEAT, ctx);

            if (key->action == IME_KEY_SHIFT) {
                /*
                 * Latch the shift key while it actually changes what a letter
                 * produces: English only (in Chinese the letters are upper case
                 * anyway and shift does nothing), and for the one-shot as well as
                 * the lock, because the next letter is upper case in both.
                 */
                const bool active = (ctx->lang == IME_LANG_EN) &&
                                    (ime_session_shift() != IME_SHIFT_OFF);
                lv_obj_set_state(btn, LV_STATE_CHECKED, active);
            }

            if (key->action == IME_KEY_BACKSPACE) {
                /*
                 * ★ PRESS_LOCK: the slide-to-clear gesture moves the finger off
                 *   this key, and without the lock LVGL re-hit-tests the finger
                 *   position on every press cycle (lv_indev.c pointer_search_obj),
                 *   hands the events to whatever is under it and sends PRESS_LOST
                 *   here - the gesture would die half way. With the lock this key
                 *   stays the active object until the finger lifts.
                 */
                lv_obj_add_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
                lv_obj_add_event_cb(btn, bksp_pressed_cb, LV_EVENT_PRESSED, ctx);
                lv_obj_add_event_cb(btn, bksp_pressing_cb, LV_EVENT_PRESSING, ctx);
                lv_obj_add_event_cb(btn, bksp_long_cb, LV_EVENT_LONG_PRESSED, ctx);
                lv_obj_add_event_cb(btn, bksp_hold_end_cb, LV_EVENT_RELEASED, ctx);
                lv_obj_add_event_cb(btn, bksp_hold_end_cb, LV_EVENT_PRESS_LOST, ctx);
            }
        }
    }
}

void ime_ui_rebuild_keyboard(lv_pinyin_ime_ctx_t *ctx)
{
    const ime_keyboard_t *kb = ime_ui_active_keyboard(ctx);
    if (kb == NULL) {
        IME_LOGW("lv_pinyin_ime", "no keyboard for mode %d panel %d", (int)ctx->mode,
                 (int)ctx->panel);
        return;
    }
    ime_ui_apply_keyboard(ctx, kb);
    ime_ui_apply_sizes(ctx);    /* fit 模式：重建后按可见行数重新分摊行高 */
}
