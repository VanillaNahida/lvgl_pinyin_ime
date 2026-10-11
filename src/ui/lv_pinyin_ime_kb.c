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
#include "mic_icon.h"   /* 语音键的位图麦克风（assets/mic.png 烘出来的，见 gen_mic_icon.py） */

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
 * R4: ?123(2u) ⌨(2u) 空格 语音 中/英(1.5u) ✓(1.5u)
 *
 * The space bar sits between the keyboard switch and the language key, which
 * puts it under the thumbs and leaves the two mode keys in the outer corners -
 * the arrangement the reference layout uses.
 *
 * The voice key (2026-11) costs the space bar one unit: the two corner keys
 * give up half a unit each and the space bar one, so the row is still exactly
 * 12 units and every column still lines up with the letter rows above. Without
 * voice input enabled the row is the old 2u/2u/4u/2u/2u one.
 */
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
static const ime_key_t s_k26_r4[] = {
    KWT("?123", NULL, IME_KEY_PANEL, 20),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 20),
    KWT("空格", NULL, IME_KEY_SPACE, 30),
    KWT("中", NULL, IME_KEY_LANG, 15),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 15),
};
#else
static const ime_key_t s_k26_r4[] = {
    KWT("?123", NULL, IME_KEY_PANEL, 20),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 20),
    KWT("空格", NULL, IME_KEY_SPACE, 40),
    KWT("中", NULL, IME_KEY_LANG, 20),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 20),
};
#endif

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

/* R4: 选拼音 语音 空格 */
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE && CONFIG_LV_PINYIN_IME_VOICE_KEY_K9
static const ime_key_t s_k9_r4[] = {
    KWT("选拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 24),
    KWT("空格", NULL, IME_KEY_SPACE, 48),
};
#else
static const ime_key_t s_k9_r4[] = {
    KWT("选拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 24),
    KWT("空格", NULL, IME_KEY_SPACE, 72),
};
#endif

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

/* 语音页中央圆形键的直径（像素）*/
#ifndef IME_UI_VOICE_D
#define IME_UI_VOICE_D 72
#endif

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

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/*
 * 语音页（P4）：整页就一个大麦克风键 + 返回。
 *
 * 为什么改成"一整页"：语音**不再占用主键盘上的一个键**（用户要求）—— 键盘上的
 * 键位本来就紧张，而且语音是"另一种输入方式"，和 26/9 键一样应该是一整页。
 * 入口是 ⌨ 键（见 lv_pinyin_ime.c 的 IME_KEY_SWITCH_KB：26键 → 9键 → 语音页 → 26键）。
 *
 * 键面：action == IME_KEY_VOICE 时 build_key() 用 mic.png 位图绘制（label 字段忽略），
 * 所以这里第一个字段留空串；按下/松手语义复用语音键那套（TOGGLE 点一下开始/再点结束，
 * PTT 按住说话松手结束，长按=我说完了）。
 */
static const ime_key_t s_voice_r1[] = {
    /* 左：返回主键盘（走现成的面板切换：text "main"）。 */
    KWT("返回", "main", IME_KEY_PANEL, 34),
    /*
     * 中：麦克风。这一格**不靠 flex 排版** —— build_key() 里给它加了 FLOATING
     * 并用 lv_obj_align(LV_ALIGN_CENTER) 定位，所以它是**屏幕正中的圆形**键，
     * 而不是被拉伸的横条（用户要求："中间显示圆形语音键"）。
     * 键面由 mic_icon（mic.png 位图）绘制，放大约 2 倍填满圆。
     */
    KWT("", NULL, IME_KEY_VOICE, 33),
};
ROWS(s_voice_rows, s_voice_kb, ROW(s_voice_r1));
#endif
static const ime_keyboard_t *const s_main_kb[IME_MODE_LAST][IME_LANG_LAST] = {
    {&s_k26_cn_kb, &s_k26_en_kb},
    {&s_k9_kb, &s_k9_kb},
};

static const ime_keyboard_t *const s_panel_kb[LV_PINYIN_IME_PANEL_LAST] = {
    NULL, /* P0 is selected by mode / language */
    &s_p1_kb,
    &s_p2_kb,
    &s_p3_kb,
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    &s_voice_kb, /* P4: 语音页 */
#endif
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
        return s_panel_kb[ctx->panel] ? s_panel_kb[ctx->panel] : &s_k26_cn_kb;
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
     * ★ 需求（2026-10）：⌨ 键**短按 = 开关语音输入**（原来短按是切 K26/K9 布局）。
     *
     *   - 语音**没接上**（组件没开语音、或宿主没 attach）时，整段跳过 →
     *     行为与以前完全一样（切布局），这样开了语音开关也不会莫名其妙失去布局切换；
     *   - 语音接了：短按 toggle 识别（开始/结束由 VOICE_TAP_MODE 决定），
     *     布局切换改到**长按**（见 key_long_press_cb 的 IME_KEY_SWITCH_KB）。
     */
    if (key->action == IME_KEY_SWITCH_KB) {
        /* （原样：CLICKED 交给 ime_ui_handle_key() 去切 K26/K9 布局。
         *   2026-10 曾一度把这里改成"短按开语音"，方向错了 —— 语音要走**独立页面**，
         *   不是抢这个键，已撤回。） */
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
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    case IME_KEY_VOICE:
        /*
         * 长按 = "我说完了" -> 结束本次识别（不取消：已经识别的内容要留下）。
         *
         * ★ 只结束、不取消。早期版本这里调的是 cancel，结果是长按说一句话什么
         *   都不会上屏 —— 取消的语义是"丢弃"（见 lv_pinyin_ime_voice.h），而用户
         *   按下长按想表达的是"我说完了"。
         *
         * ★ 只在**已经开始**时才结束：长按发生在按下之后、松手之前，而按下的那一下
         *   已经开了会话。这里只管收尾，重复调用 stop 是幂等的（会话不在跑就返回）。
         */
        if (lv_pinyin_ime_voice_of(ctx->obj) != NULL) {
            lv_pinyin_ime_voice_key_released(ctx->obj);
        }
        break;
#endif
    default:
        break;
    }
}

/* ------------------------------------------------- backspace: hold + gesture */

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/*
 * 语音键：按下开始、松手结束。
 *
 * ★ 不能像其它键那样靠 CLICKED：对讲模式（PTT）要的是"按住说话、松手结束"，
 *   而 CLICKED 只在松手时才发一次，压不住"按下"这个时刻。而 TOGGLE 模式只需要
 *   按下那一下（再按一次结束），所以用 PRESSED 统一入口、RELEASED 收尾：
 *
 *     按下  → 空闲则开始识别；正在识别则什么都不做（等松手决定）
 *     松手  → TOGGLE：没变化（识别继续，等下一次按下才结束）
 *             PTT   ：结束识别，结果上屏
 *     长按  → 结束识别（三种模式一致，见 key_long_press_cb）
 *
 *   这样"短按切换"和"长按对讲"共用一条按下/松手路径，不会互相打架：TOGGLE 用户
 *   按一下就走，手指抬起不会误结束；PTT 用户一直按着，松手才结束。
 */
static void voice_pressed_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->obj == NULL) {
        return;
    }
    lv_pinyin_ime_voice_key_pressed(ctx->obj);
}

static void voice_released_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->obj == NULL) {
        return;
    }
    lv_pinyin_ime_voice_key_released(ctx->obj);
}

/** 按键失去焦点（滑走、被别处抢走）：对讲模式下按"松手"处理，别把麦克风开着。 */
static void voice_press_lost_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->obj == NULL) {
        return;
    }
    lv_pinyin_ime_voice_key_released(ctx->obj);
}
#endif

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

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/* 语音键的按下/松手/长按 —— 与主键盘上那个语音键走同一套组件 API。 */
/*
 * ⚠⚠ user_data 挂的是 **ctx**（不是 ime 对象）。组件的语音 API 要的是**键盘根对象**，
 *    所以必须走 ctx->obj —— 直接把 user_data 强转成 lv_obj_t* 传进去，等于把 ctx
 *    当对象用：组件在它上面找不到语音会话，表现就是**点了完全没反应**
 *    （2026-10 真机踩过，这版修掉）。
 */
static void voice_page_mic_pressed(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)lv_event_get_user_data(e);
    if (ctx != NULL) {
        lv_pinyin_ime_voice_key_pressed(ctx->obj);
    }
}
static void voice_page_mic_released(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)lv_event_get_user_data(e);
    if (ctx != NULL) {
        lv_pinyin_ime_voice_key_released(ctx->obj);
    }
}
static void voice_page_mic_long(lv_event_t *e)
{
    voice_page_mic_released(e);
}

/* 底部左键：回 26 键主键盘 */
static void voice_page_back_kb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)lv_event_get_user_data(e);
    ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
    ctx->panel_alt = false;
    ctx->mode = IME_MODE_K26;
    ime_session_set_mode(ctx->mode);
    ime_ui_rebuild_keyboard(ctx);
}

/* 底部右键：结束/取消本次识别并回主键盘 */
static void voice_page_back_done(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)lv_event_get_user_data(e);
    lv_pinyin_ime_voice_key_cancelled(ctx->obj);   /* 会话没在跑时是空操作 */
    ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
    ctx->panel_alt = false;
    ime_ui_rebuild_keyboard(ctx);
}

/*
 * 语音页的版式（用户要求）：
 *
 *     ┌────────────────────────────┐
 *     │      聆听中 / 识别中 / 原因    │  ← 状态文字（voice_page_status）
 *     │            ◯ 🎤             │  ← 正中圆形麦克风（FLOATING 都不要，见下）
 *     │      ⌨            返回        │  ← 底部一行
 *     └────────────────────────────┘
 *
 * ⚠ 为什么这里**不复用 ime_ui_apply_keyboard() 的键表渲染**：那一套把每个键放进
 *   flex 行、宽度由权重决定，圆形按钮会被拉成横条（上一版就是这么坏的），而且
 *   FLOATING + align 的 hack 会让按钮浮到键盘上遮住按键。这里全部自己搭、用百分比
 *   与固定尺寸，位置确定。
 */
static void voice_page_build(lv_pinyin_ime_ctx_t *ctx)
{
    /* 状态文字 */
    lv_obj_t *st = lv_label_create(ctx->kb_rows);
    ime_style_apply_font(st, ime_font_small());
    lv_obj_set_width(st, LV_PCT(100));
    lv_obj_set_height(st, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(st, "语音输入");
    ctx->voice_page_status = st;

    /* 正中圆形麦克风：用一个撑满剩余高度的容器 + 居中，位置由布局决定而不是绝对坐标 */
    lv_obj_t *mid = lv_obj_create(ctx->kb_rows);
    lv_obj_remove_style_all(mid);
    lv_obj_set_width(mid, LV_PCT(100));
    /*
     * ⚠ 这里**必须给确定高度**，不能用 flex_grow：父容器 ctx->kb_rows 是
     *   LV_SIZE_CONTENT（高度由内容决定），在它里面 grow 没有任何空间可分 ——
     *   实测结果就是容器高度算成 0、居中的圆形按钮整颗消失（2026-10 真机）。
     *   高度取"圆直径"，上下不再加 padding，整页高度 = 状态行 + 圆 + 底部行。
     */
    /* 父容器（kb_rows）在语音页被撑成 100%，所以这里 grow 是有效的；min_height 兜底，
     * 万一父高度算不出来也不会塌成 0（上一版就是塌成 0 → 圆消失）。 */
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_style_min_height(mid, IME_UI_VOICE_D, LV_PART_MAIN);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mic = lv_button_create(mid);
    ime_style_apply_key(mic, false);
    lv_obj_set_size(mic, IME_UI_VOICE_D, IME_UI_VOICE_D);
    lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_center(mic);
    lv_obj_t *icon = lv_image_create(mic);
    lv_image_set_src(icon, &mic_icon);
    lv_image_set_scale(icon, 512);            /* 24px 位图放大 2 倍 */
    lv_obj_center(icon);
    lv_obj_add_event_cb(mic, voice_page_mic_pressed, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(mic, voice_page_mic_released, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(mic, voice_page_mic_long, LV_EVENT_LONG_PRESSED, ctx);
    ctx->voice_page_mic = mic;

    /* 底部一行：左 ⌨、右 返回 */
    lv_obj_t *row = lv_obj_create(ctx->kb_rows);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kb_btn = lv_button_create(row);
    ime_style_apply_key(kb_btn, true);
    lv_obj_set_size(kb_btn, IME_UI_VOICE_D * 2, IME_UI_VOICE_D / 2);
    lv_obj_t *kb_lab = lv_label_create(kb_btn);
    ime_style_apply_font(kb_lab, LV_FONT_DEFAULT);
    lv_label_set_text(kb_lab, LV_SYMBOL_KEYBOARD);
    lv_obj_center(kb_lab);
    lv_obj_add_event_cb(kb_btn, voice_page_back_kb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *back_btn = lv_button_create(row);
    ime_style_apply_key(back_btn, false);
    lv_obj_set_size(back_btn, IME_UI_VOICE_D * 2, IME_UI_VOICE_D / 2);
    lv_obj_t *back_lab = lv_label_create(back_btn);
    ime_style_apply_font(back_lab, ime_font_small());
    lv_label_set_text(back_lab, "返回");
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, voice_page_back_done, LV_EVENT_CLICKED, ctx);
}
#endif

void ime_ui_apply_keyboard(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb)
{
    lv_obj_clean(ctx->kb_rows);
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
        /*
         * 语音页：自己搭；候选栏让位（状态由上面那行文字承担）。
         *
         * ★ kb_rows 平时是 LV_SIZE_CONTENT（高度=各行之和），语音页内容少（约 135px），
         *   而键盘区是固定高度 —— 差出来的那截就成了输入框下面那段**大白**（用户报）。
         *   所以语音页把 kb_rows 撑满，让中间的圆形键去吃掉多余空间（它里面有 flex_grow）。
         */
        lv_obj_set_height(ctx->kb_rows, LV_PCT(100));
        if (ctx->cand_bar != NULL) {
            lv_obj_add_flag(ctx->cand_bar, LV_OBJ_FLAG_HIDDEN);
        }
        voice_page_build(ctx);
        return;
    }
    lv_obj_set_height(ctx->kb_rows, LV_SIZE_CONTENT);   /* 其它页恢复原样 */
    if (ctx->cand_bar != NULL) {
        lv_obj_remove_flag(ctx->cand_bar, LV_OBJ_FLAG_HIDDEN);
    }
#endif

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

            /*
             * 键面：语音键用**位图麦克风**（assets/mic.png → src/ui/mic_icon.c），
             * 其余键照旧用文字 label。
             *
             * 为什么不用 LV_SYMBOL_AUDIO：那是 FontAwesome 的**喇叭**，表达的是
             * "播放/音量"而不是"说话"，用户看着不对；而且它靠字体里刚好有那个
             * 私用区字形，换了 UI 字体就会变成方框。位图与字体无关。
             *
             * 注：键面文字只在建键盘时写一次（见上面 lv_label_set_text 的唯一调用点），
             * 没有"运行时改键面文字"的路径，所以这里换成 image 子对象是安全的。
             */
            if (key->action == IME_KEY_VOICE) {
                lv_obj_t *icon = lv_image_create(btn);
                lv_image_set_src(icon, &mic_icon);
                /* 语音页那颗键：做成屏幕正中的大圆形（用户要求）。
                 * FLOATING = 脱离父容器 flex 布局，再由 lv_obj_align 居中；
                 * 不这么做的话它会被 flex 拉伸成一条横键（原先是 70% 宽的长条）。 */
                lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
                lv_obj_set_size(btn, IME_UI_VOICE_D, IME_UI_VOICE_D);
                lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
                /* ⚠ 居中的参照物必须是**键盘根对象**（ctx->obj），不能是 btn 的父对象
                 * （那一行只有 40 多 px 高，84 px 的圆会溢出被裁）。 */
                lv_obj_align_to(btn, ctx->obj, LV_ALIGN_CENTER, 0, 0);
                /* 位图 24x24 放进 84px 的圆里偏小：放大 2 倍（LVGL 的缩放单位是 1/256） */
                lv_image_set_scale(icon, 512);
                lv_obj_center(icon);
            } else {
                lv_obj_t *label = lv_label_create(btn);
                lv_label_set_text(label, key_label(ctx, key));
                lv_label_set_recolor(label, true);
                ime_style_apply_font(label, key_caption_font(key));
                lv_obj_center(label);
            }

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

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
            if (key->action == IME_KEY_VOICE) {
                /*
                 * 语音键走"按下 / 松手"而不是 CLICKED —— 对讲模式需要"按下"这一刻
                 * （CLICKED 只在松手时来一次）。按下那一下同时也注册了 CLICKED，
                 * 这里不去管它：handle_key 对 IME_KEY_VOICE 是空操作（见
                 * lv_pinyin_ime.c），不会重复触发。
                 */
                lv_obj_add_event_cb(btn, voice_pressed_cb, LV_EVENT_PRESSED, ctx);
                lv_obj_add_event_cb(btn, voice_released_cb, LV_EVENT_RELEASED, ctx);
                lv_obj_add_event_cb(btn, voice_press_lost_cb, LV_EVENT_PRESS_LOST, ctx);
            }
#endif
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
