/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The key tables, transcribed from docs/keymap.md. See ime_keymaps.h for the
 * width semantics (CENTER = tenths of the standard key width, FILL = a ratio
 * inside its own row) and for why this is a separate translation unit.
 */

#include "ime_keymaps.h"

#include "lvgl.h"   /* LV_SYMBOL_* only: the captions of the special keys */

#define K(label, action) {label, NULL, 10, action}
#define KW(label, action, w) {label, NULL, w, action}
#define KT(label, text, action) {label, text, 10, action}
#define KWT(label, text, action, w) {label, text, w, action}

/* --------------------------------------------------------------- 26 key */

/*
 * 26 key. Twelve units per row, and the reference row is R1 (see ime_kb_k26_cn):
 *
 *   R1  符(1.5u) + 10 letters(0.9u) + ⌫(1.5u)  = 12.0u   <- defines U
 *   R2  ⇧(1.5u) +  9 letters(0.9u) + ⏎(1.5u)   = 11.1u   centered
 *   R3  分词(1.5u) - z..m . , :               = 11.4u   centered
 *   R4  ?123(2u) ⌨(2u) 空格(3u) 中(2.5u) ✓(2.5u) = 12.0u  filled
 *
 * R2 and R3 are *deliberately* not stretched to 12u: a real phone keyboard keeps
 * the letter keys the same size on every row and lets the shorter rows sit
 * centered, which is what produces the familiar indent. Forcing every row to sum
 * to 120 (which the old comments asked for) would have made the letters on R3
 * narrower than the ones on R1 - the exact opposite of "columns line up".
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
 * R4: ?123(2u) ⌨(2u) 空格(3u) 中/英(2.5u) ✓(2.5u) = 12u.
 *
 * FILL mode: the numbers below are ratios inside this row, not standard key
 * widths, and 20 + 20 + 30 + 25 + 25 = 120 keeps the row on the same 12u grid as
 * the letter rows above.
 *
 * ★ 这里原来有一对 #if CONFIG_LV_PINYIN_IME_VOICE_ENABLE 分支，语音开的时候是
 *   20/20/30/15/15 = 100（少了整整 2u，所以空格与右侧两个键比别的行窄一截），
 *   语音关的时候是 20/20/40/20/20 = 120。两个分支都不是"语音要不要占一个键"决定的
 *   —— 语音早就不在主键盘上了，它是**独立的一页**（PANEL_VOICE，入口是 ⌨ 键）。
 *   所以两套数值没有任何理由不同，合并成一套，并把空格缩 1u、两侧功能键各加 0.5u，
 *   正好补回那缺掉的 2u。
 */
static const ime_key_t s_k26_r4[] = {
    KWT("?123", NULL, IME_KEY_PANEL, 20),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 20),
    KWT("空格", NULL, IME_KEY_SPACE, 30),
    KWT("中", NULL, IME_KEY_LANG, 25),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 25),
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
 * 9 key. Five keys of 2.4u per row = 12u, the same grid width as the 26-key
 * keyboard, so the four rows line up among themselves and the digit columns are
 * the same width on every row.
 *
 * R1-R3 are CENTER (2.4u keys are 2.4x a letter key - that *is* the standard key
 * width here, which is what keeps the keys from being stretched by the row's key
 * count), R4 is FILL.
 */
static const ime_key_t s_k9_r1[] = {
    KWT("123", NULL, IME_KEY_PANEL, 24),
    KWT(",。?!", NULL, IME_KEY_PANEL, 24),
    KWT("ABC", NULL, IME_KEY_DIGIT, 24), KWT("DEF", NULL, IME_KEY_DIGIT, 24),
    KWT(LV_SYMBOL_BACKSPACE, NULL, IME_KEY_BACKSPACE, 24),
};

/* R2: 中 GHI JKL MNO 分隔 */
static const ime_key_t s_k9_r2[] = {
    KWT("中", NULL, IME_KEY_LANG, 24),
    KWT("GHI", NULL, IME_KEY_DIGIT, 24), KWT("JKL", NULL, IME_KEY_DIGIT, 24),
    KWT("MNO", NULL, IME_KEY_DIGIT, 24),
    KWT("分隔", NULL, IME_KEY_SEPARATOR, 24),
};

/* R3: 拼音 PQRS TUV WXYZ ✓ */
static const ime_key_t s_k9_r3[] = {
    KWT("拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT("PQRS", NULL, IME_KEY_DIGIT, 24), KWT("TUV", NULL, IME_KEY_DIGIT, 24),
    KWT("WXYZ", NULL, IME_KEY_DIGIT, 24),
    KWT(LV_SYMBOL_OK, NULL, IME_KEY_CONFIRM, 24),
};

/*
 * R4: 选拼音(2.4u) ⌨(2.4u) 空格(7.2u) = 12u -> FILL ratios 20/20/60.
 *
 * ★ 原来这里有一个 #if CONFIG_LV_PINYIN_IME_VOICE_ENABLE && CONFIG_LV_PINYIN_IME_VOICE_KEY_K9
 *   分支，把空格从 72 缩到 48（24+24+48 = 96，比别的行少 2.4u）。两个问题：
 *
 *   1. CONFIG_LV_PINYIN_IME_VOICE_KEY_K9 **在 Kconfig 里根本不存在**，从来没有被
 *      定义过 —— 那个分支是死代码，"语音开的时候九键底部行会窄一截"从来没发生过。
 *   2. 就算它生效也说不通：那一行里没有 IME_KEY_VOICE，缩窄空格并不会变出语音键来。
 *
 *   九键的语音入口和 26 键一样是 ⌨ 键切到语音页，所以这里只有一个正确的数值。
 */
static const ime_key_t s_k9_r4[] = {
    KWT("选拼音", NULL, IME_KEY_T9_PINYIN, 24),
    KWT(LV_SYMBOL_KEYBOARD, NULL, IME_KEY_SWITCH_KB, 24),
    KWT("空格", NULL, IME_KEY_SPACE, 72),
};

/* ------------------------------------------------------- P1 digits + symbols */

/*
 * The panel behind "符 / ?123". Row 1 is the digits, row 2 the punctuation the IME
 * emits most, row 3 the panel navigation, and the last row the usual bottom bar.
 *
 * There used to be two separate panels here (P1 "digits + common symbols" and P2
 * "digits only") reached by two different keys, but both opened the same layout,
 * so they are one panel and one key now.
 *
 * All rows are FILL: ten equal keys across 10u, or the functional 4u/4u/2u and
 * 2u/2u/4u/2u splits - the ratios are what matters, and FILL makes each row span
 * exactly the same left and right edges.
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

/*
 * Three columns of 1u each (FILL: 10/10/10). It keeps its own three column grid
 * on purpose - only the outer edges and the row height are shared with the other
 * panels, which is what the guide asks for (§7.4).
 */
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

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/*
 * 语音页（P4）：一整页就一个大麦克风 + 底部两个按钮。
 *
 * ★ 这张表**不参与渲染** —— 语音页由 kb.c 的 voice_page_build() 自己搭（三区段：
 *   状态文字 / 圆形麦克风 / 底部操作栏，尺寸全部由 ime_layout_measure_ref() 按同一个
 *   12u 网格算出来），因为圆形麦克风不是"一行里的一个键"。
 *   留着它只是为了让 PANEL_VOICE 在 ime_ui_active_keyboard() 里有一个合法的表
 *   （表校验、日志、以及将来真要做成普通键表时不必再补），两个 6u 键正好对应底部
 *   那两个按钮的宽度。
 */
static const ime_key_t s_voice_r1[] = {
    KWT("返回", "main", IME_KEY_PANEL, 60),
    KWT("", NULL, IME_KEY_VOICE, 60),
};
#endif

/* ------------------------------------------------------------------ tables */

#define ROW(keys, alignment) {keys, sizeof(keys) / sizeof(keys[0]), alignment}

#define ROWS(rows_name, kb_name, ref, ...)                            \
    static const ime_row_t rows_name[] = {__VA_ARGS__};               \
    const ime_keyboard_t kb_name = {                                  \
        rows_name, sizeof(rows_name) / sizeof(rows_name[0]), ref}

ROWS(s_k26_cn_rows, ime_kb_k26_cn, 0,
     ROW(s_k26_r1, IME_ROW_ALIGN_CENTER), ROW(s_k26_r2, IME_ROW_ALIGN_CENTER),
     ROW(s_k26_r3, IME_ROW_ALIGN_CENTER), ROW(s_k26_r4, IME_ROW_ALIGN_FILL));
ROWS(s_k26_en_rows, ime_kb_k26_en, 0,
     ROW(s_k26_r1, IME_ROW_ALIGN_CENTER), ROW(s_k26_r2, IME_ROW_ALIGN_CENTER),
     ROW(s_k26_en_r3, IME_ROW_ALIGN_CENTER), ROW(s_k26_r4, IME_ROW_ALIGN_FILL));

ROWS(s_k9_rows, ime_kb_k9, 0,
     ROW(s_k9_r1, IME_ROW_ALIGN_CENTER), ROW(s_k9_r2, IME_ROW_ALIGN_CENTER),
     ROW(s_k9_r3, IME_ROW_ALIGN_CENTER), ROW(s_k9_r4, IME_ROW_ALIGN_FILL));

ROWS(s_p1_rows, ime_kb_p1, 0,
     ROW(s_p1_r1, IME_ROW_ALIGN_FILL), ROW(s_p1_r2, IME_ROW_ALIGN_FILL),
     ROW(s_p1_r3, IME_ROW_ALIGN_FILL), ROW(s_p1_r4, IME_ROW_ALIGN_FILL));
ROWS(s_p1_alt_rows, ime_kb_p1_alt, 0,
     ROW(s_p1a_r1, IME_ROW_ALIGN_FILL), ROW(s_p1a_r2, IME_ROW_ALIGN_FILL),
     ROW(s_p1_r3, IME_ROW_ALIGN_FILL), ROW(s_p1_r4, IME_ROW_ALIGN_FILL));
ROWS(s_p2_rows, ime_kb_p2, 0,
     ROW(s_p2_r1, IME_ROW_ALIGN_FILL), ROW(s_p2_r2, IME_ROW_ALIGN_FILL),
     ROW(s_p2_r3, IME_ROW_ALIGN_FILL), ROW(s_p2_r4, IME_ROW_ALIGN_FILL));
ROWS(s_p3_rows, ime_kb_p3, 0,
     ROW(s_p3_r1, IME_ROW_ALIGN_FILL), ROW(s_p3_r2, IME_ROW_ALIGN_FILL),
     ROW(s_p3_r3, IME_ROW_ALIGN_FILL), ROW(s_p3_r4, IME_ROW_ALIGN_FILL),
     ROW(s_p3_r5, IME_ROW_ALIGN_FILL));

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
ROWS(s_voice_rows, ime_kb_voice, 0, ROW(s_voice_r1, IME_ROW_ALIGN_CENTER));
#endif

const ime_keymap_entry_t ime_keymaps[] = {
    {"k26_cn", &ime_kb_k26_cn},
    {"k26_en", &ime_kb_k26_en},
    {"k9", &ime_kb_k9},
    {"p1", &ime_kb_p1},
    {"p1_alt", &ime_kb_p1_alt},
    {"p2", &ime_kb_p2},
    {"p3", &ime_kb_p3},
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    {"voice", &ime_kb_voice},
#endif
};

const size_t ime_keymaps_count = sizeof(ime_keymaps) / sizeof(ime_keymaps[0]);
