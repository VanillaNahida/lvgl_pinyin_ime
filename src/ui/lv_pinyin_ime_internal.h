/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private state shared by the UI translation units. Nothing here is part of
 * the public API: include/lvgl_pinyin_ime/ stays free of internals.
 */

#pragma once

#include "lvgl.h"

#include "core/ime_session.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_types.h"
#include "lv_pinyin_ime_style.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Height of one key row / candidate row in pixels.
 *
 * 28 keeps the keys close to square: a 26-key letter key is about 20 px wide, and
 * the reference layout's keys are only slightly taller than wide. Four of these
 * plus the candidate bar and the widget padding still fit the shortest supported
 * screen (240 px).
 */
#define IME_UI_ROW_HEIGHT 28

/**
 * Key layout grid. A row spans IME_UI_ROW_UNITS units; each key is placed on that
 * grid rather than stretched by flexbox, so the gaps are even and the columns
 * line up between rows. These are shared with the unit calculation in
 * lv_pinyin_ime.c so the two cannot drift apart.
 */
#define IME_UI_ROW_UNITS 12
#define IME_UI_KEY_GAP 3
#define IME_UI_PAD 2

/** Candidates drawn at once in the candidate bar (one page). */
#define IME_UI_CAND_MAX 12

/** Pinyin chips drawn at once in the 9-key pinyin row. */
#define IME_UI_T9_PINYIN_MAX 8

/** Width of the layout unit `u` in pixels, for the active screen. */
extern int IME_UI_UNIT_PX;

/** What a key does when pressed. */
typedef enum {
    IME_KEY_NONE = 0,
    IME_KEY_LETTER,     /**< ASCII letter, `label` holds it */
    IME_KEY_DIGIT,      /**< 9-key digit group, `label` is "ABC".."WXYZ" */
    IME_KEY_TEXT,       /**< commit `text` verbatim */
    IME_KEY_FULLWIDTH,  /**< commit `label`, full width in Chinese mode */
    IME_KEY_BACKSPACE,
    IME_KEY_ENTER,
    IME_KEY_SPACE,
    IME_KEY_SHIFT,
    IME_KEY_CONFIRM,
    IME_KEY_SEPARATOR,  /**< 分词 / 分隔: insert '\'' */
    IME_KEY_SWITCH_KB,  /**< 26-key <-> 9-key */
    IME_KEY_LANG,       /**< toggle Chinese / English */
    IME_KEY_PANEL,      /**< open the panel named in `text` */
    IME_KEY_T9_PINYIN,  /**< 选拼音: show / hide the 9-key pinyin row */
} ime_key_action_t;

/** One key of a keyboard row. */
typedef struct {
    const char *label;
    const char *text;       /**< payload for TEXT / PANEL actions */
    uint8_t width_u;        /**< width in tenths of a unit, 10 = 1u */
    ime_key_action_t action;
} ime_key_t;

/** One row of a keyboard. */
typedef struct {
    const ime_key_t *keys;
    size_t count;
} ime_row_t;

/** A complete keyboard. */
typedef struct {
    const ime_row_t *rows;
    size_t row_count;
} ime_keyboard_t;

#ifndef IME_UI_PINYIN_TEXT_MAX
#define IME_UI_PINYIN_TEXT_MAX 96
#endif

typedef struct {
    lv_obj_t *obj;    /**< the IME root object */
    lv_obj_t *ta;     /**< bound text area, may be NULL */

    ime_mode_t mode;
    ime_lang_t lang;
    lv_pinyin_ime_panel_t panel;
    bool panel_alt;        /**< P1 is showing the "other symbols" batch */
    bool t9_row_visible;   /**< 选拼音 toggle */

    /** Composition text with one apostrophe per syllable boundary ("la'wan'le"). */
    char pinyin_text[IME_UI_PINYIN_TEXT_MAX];

    /* candidate bar */
    lv_obj_t *cand_bar;
    lv_obj_t *cand_row;
    lv_obj_t *page_prev;
    lv_obj_t *page_next;
    lv_obj_t *chip;
    lv_obj_t *cand_btns[IME_UI_CAND_MAX];
    lv_obj_t *cand_labels[IME_UI_CAND_MAX];

    /* 9-key pinyin row */
    lv_obj_t *t9_row;
    lv_obj_t *t9_btns[IME_UI_T9_PINYIN_MAX];
    lv_obj_t *t9_labels[IME_UI_T9_PINYIN_MAX];

    /* keyboard */
    lv_obj_t *kb_rows;
} lv_pinyin_ime_ctx_t;

/** State pointer of a root object, or NULL. */
lv_pinyin_ime_ctx_t *ime_ctx_get(lv_obj_t *obj);

/** lv_event callback that frees the context together with the root object. */
void ime_root_delete_cb(lv_event_t *e);

/* ------------------------------------------------------------- candidates */

/** Create the candidate bar, the pinyin chip and the 9-key pinyin row. */
void ime_ui_build_candidates(lv_pinyin_ime_ctx_t *ctx);

/** Redraw the candidate list, the chip and the pager state. */
void ime_ui_refresh_candidates(lv_pinyin_ime_ctx_t *ctx);

/** Redraw the 9-key pinyin row. */
void ime_ui_refresh_t9(lv_pinyin_ime_ctx_t *ctx);

/* --------------------------------------------------------------- keyboard */

/** Rebuild the key rows for the current mode / panel / language. */
void ime_ui_rebuild_keyboard(lv_pinyin_ime_ctx_t *ctx);

/** Keyboard description for the active mode / panel / language. */
const ime_keyboard_t *ime_ui_active_keyboard(const lv_pinyin_ime_ctx_t *ctx);

/** Create the buttons of one keyboard. */
void ime_ui_apply_keyboard(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb);

/* ------------------------------------------------------------------- misc */

/**
 * Move every pending commit from the session into the bound text area.
 *
 * The session buffers committed text and the UI drains it in one place. Any
 * interaction that can commit (a key press *or* a candidate tap) has to call
 * this, or the text sits in the buffer until some later key press flushes it -
 * which looks like "the word only appears after I press another key".
 */
void ime_ui_commit_pending(lv_pinyin_ime_ctx_t *ctx);

/** Refresh everything the session can change. */
void ime_ui_refresh_all(lv_pinyin_ime_ctx_t *ctx);

/** Dispatch one key press. */
void ime_ui_handle_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key);

/** Map a T9 key caption ("ABC") to its digit; '0' when unknown. */
char ime_ui_digit_for_group(const char *caption);

/** Full width form of a punctuation label in Chinese mode. */
const char *ime_ui_fullwidth(const lv_pinyin_ime_ctx_t *ctx, const char *label);

/** One shot / lock / off cycle for the shift key. */
void ime_ui_cycle_shift(lv_pinyin_ime_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
