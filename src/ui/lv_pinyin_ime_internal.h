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
 * Default height of one key row / candidate row in pixels.
 *
 * 28 keeps the keys close to square: a 26-key letter key is about 20 px wide, and
 * the reference layout's keys are only slightly taller than wide. Four of these
 * plus the candidate bar and the widget padding still fit the shortest supported
 * screen (240 px).
 *
 * The height actually used at build time is IME_UI_ROW_HEIGHT_PX, which
 * lv_pinyin_ime_set_row_height() overrides. A large panel wants taller rows:
 * with the default, the 5-row 26-key layout is only about 150 px tall, which on
 * a 480 px screen leaves the keys small and hard to hit.
 */
#define IME_UI_ROW_HEIGHT_DEFAULT 28

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

/* ------------------------------------------------- backspace hold + gesture */

/**
 * Caption of the hold-to-clear hint.
 *
 * Shown while the backspace key is held down; sliding the finger left by about
 * the width of the pill (that is, reaching its far end) clears the text area.
 * Every character here is inside the IME font subset (GB2312 + UI symbols), so
 * the hint never renders as boxes - if you change the text, check it against
 * generated/charset.txt first (tools/check_charset.py).
 */
#define IME_UI_BKSP_HINT_TEXT "← 左滑清空输入框"

/** Lower bound of the slide distance that clears the text area (px). */
#define IME_UI_BKSP_CLEAR_MIN 72

/** Upper bound of that distance: on a wide screen the pill must not demand a
 *  slide across half the keyboard. */
#define IME_UI_BKSP_CLEAR_MAX 240

/** Width of the layout unit `u` in pixels, for the active screen. */
extern int IME_UI_UNIT_PX;

/**
 * Height of one key / candidate row in pixels, for the active screen.
 *
 * Initialised to IME_UI_ROW_HEIGHT_DEFAULT; lv_pinyin_ime_set_row_height()
 * changes it. Read when a row is created, so it has to be set before
 * lv_pinyin_ime_create().
 */
extern int IME_UI_ROW_HEIGHT_PX;

/**
 * Target total height of the whole widget in pixels (0 = fitting disabled).
 *
 * Set by lv_pinyin_ime_set_fit_height(). When non-zero, ime_ui_apply_sizes()
 * derives IME_UI_ROW_HEIGHT_PX from this and the *visible* row count, and
 * re-applies it to every row, so the keyboard fills the target height no matter
 * how many rows the current mode / panel needs (see ime_ui_apply_sizes).
 */
extern int IME_UI_FIT_HEIGHT_PX;

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

    /*
     * Backspace hold state: LVGL repeats LV_EVENT_LONG_PRESSED_REPEAT every
     * long_press_repeat_time (100 ms by default) while the key is held, and the
     * same hold doubles as the "slide left to clear" gesture. All of it lives
     * here so a rebuild of the key rows cannot lose it.
     */
    lv_obj_t *bksp_hint;      /**< the "← 左滑清空输入框" pill (built lazily) */
    int32_t   bksp_press_x;   /**< finger x when the backspace key went down */
    int32_t   bksp_drag_px;   /**< slide distance that clears the text area */
    bool      bksp_hold;      /**< long press reached: hint shown, gesture armed */
    bool      bksp_cleared;   /**< this hold already cleared the text area once */
    bool      bksp_consumed;  /**< a repeat ran: swallow the synthetic CLICKED */
} lv_pinyin_ime_ctx_t;

/** State pointer of a root object, or NULL. */
lv_pinyin_ime_ctx_t *ime_ctx_get(lv_obj_t *obj);

/** lv_event callback that frees the context together with the root object. */
void ime_root_delete_cb(lv_event_t *e);

/* ---------------------------------------------------------------- fit sizing */

/**
 * Re-derive the row height from IME_UI_FIT_HEIGHT_PX and the *visible* row
 * count, then apply it to the candidate bar, the 9-key pinyin row and every
 * key row. No-op when fitting is disabled (IME_UI_FIT_HEIGHT_PX <= 0).
 */
void ime_ui_apply_sizes(lv_pinyin_ime_ctx_t *ctx);

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

/**
 * Switch the input language and drop the shift state with it.
 *
 * Chinese draws its letters upper case and ignores the caps-lock key, so a lock
 * kept across a language switch could not be cleared while Chinese is active and
 * would silently come back with English. Every language change goes through
 * here so the keyboard is rebuilt exactly once, in a consistent state.
 */
void ime_ui_set_lang(lv_pinyin_ime_ctx_t *ctx, ime_lang_t lang);

/** Refresh everything the session can change. */
void ime_ui_refresh_all(lv_pinyin_ime_ctx_t *ctx);

/** Dispatch one key press. */
void ime_ui_handle_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key);

/* ------------------------------------------------------- backspace behaviour */

/**
 * Delete one character: the pinyin buffer first, then the bound text area.
 *
 * Shared by the click handler and the hold-to-repeat handler - the repeat used
 * to call ime_session_backspace() on its own, which does nothing at all once the
 * composition buffer is empty, so holding the key only ever deleted the single
 * character the release-time CLICKED removed.
 */
void ime_ui_backspace(lv_pinyin_ime_ctx_t *ctx);

/** Backspace key went down at @p finger_x: start a fresh hold. */
void ime_ui_bksp_hold_begin(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x);

/** Long press reached: show the hint and arm the slide-to-clear gesture. */
void ime_ui_bksp_hold_arm(lv_pinyin_ime_ctx_t *ctx);

/** Pointer moved to @p finger_x while held: clear once the slide is long enough. */
void ime_ui_bksp_hold_move(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x);

/** The hold ended (release or press lost): hide the hint and disarm. */
void ime_ui_bksp_hold_end(lv_pinyin_ime_ctx_t *ctx);

/**
 * True when the pending CLICKED belongs to a hold that already deleted
 * something. LVGL sends CLICKED unconditionally after a long press, so without
 * this the release would delete one character more than the user asked for.
 * Reading it clears the flag; the flag is also cleared on the next press.
 */
bool ime_ui_bksp_take_consumed(lv_pinyin_ime_ctx_t *ctx);

/** Map a T9 key caption ("ABC") to its digit; '0' when unknown. */
char ime_ui_digit_for_group(const char *caption);

/** Full width form of a punctuation label in Chinese mode. */
const char *ime_ui_fullwidth(const lv_pinyin_ime_ctx_t *ctx, const char *label);

/** One shot / lock / off cycle for the shift key. */
void ime_ui_cycle_shift(lv_pinyin_ime_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
