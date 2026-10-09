/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Session layer: the pinyin buffer, candidate list and paging cursor that sit
 * between the UI and the engine. No LVGL dependency, so it is unit tested on
 * the host.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl_pinyin_ime/ime_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Keyboard mode. */
typedef enum {
    IME_MODE_K26 = 0,
    IME_MODE_K9,
    IME_MODE_LAST,
} ime_mode_t;

/** Input language. */
typedef enum {
    IME_LANG_CN = 0,
    IME_LANG_EN,
    IME_LANG_LAST,
} ime_lang_t;

/** Shift behaviour of the 26-key keyboard. */
typedef enum {
    IME_SHIFT_OFF = 0,
    IME_SHIFT_ONCE,
    IME_SHIFT_LOCK,
} ime_shift_t;

#ifndef IME_SESSION_MAX_CANDIDATES
#define IME_SESSION_MAX_CANDIDATES 60
#endif

#ifndef IME_SESSION_CAND_PAGE_SIZE
#define IME_SESSION_CAND_PAGE_SIZE 8
#endif

#ifndef IME_SESSION_MAX_PINYIN_LEN
#define IME_SESSION_MAX_PINYIN_LEN 32
#endif

/** Result of an operation that may change what the UI has to redraw. */
typedef enum {
    IME_CHANGE_NONE = 0,   /**< nothing visible changed */
    IME_CHANGE_CANDIDATES, /**< candidate list / pinyin chip changed */
    IME_CHANGE_STATE,      /**< mode, language or keyboard layout changed */
    IME_CHANGE_COMMIT,     /**< text was committed to the target */
} ime_change_t;

/** Initialise the session. Safe to call again; keeps the engine loaded. */
bool ime_session_init(void);

/** Release the engine and all session memory. */
void ime_session_deinit(void);

/** True once the engine is loaded and the session can produce candidates. */
bool ime_session_ready(void);

/** Discard the pinyin buffer and the candidate list. */
void ime_session_reset(void);

/* ------------------------------------------------------------------ input */

/** Append one ASCII letter (a-z, A-Z) to the pinyin buffer. */
ime_change_t ime_session_push_letter(char ch);

/** Append the syllable separator '\''. */
ime_change_t ime_session_push_separator(void);

/** Remove one character: pinyin first, then the target text area. */
ime_change_t ime_session_backspace(void);

/** Commit the currently selected candidate, or pass the raw buffer through. */
ime_change_t ime_session_commit_current(void);

/** Commit candidate @p index of the current list. */
ime_change_t ime_session_commit_candidate(size_t index);

/** True when something is committed and @p out receives it (UTF-8). */
bool ime_session_take_commit(char *out, size_t out_size);

/** True when a commit is waiting to be collected. */
bool ime_session_has_commit(void);

/** Commit a literal string (punctuation, space, symbol) to the target. */
void ime_session_commit_literal(const char *text);

/** True while a pinyin buffer or a candidate list is active. */
bool ime_session_has_input(void);

/* -------------------------------------------------------------- candidates */

/**
 * How many candidates fit on one screen, set by the UI.
 *
 * The UI measures the candidate texts (a phrase like 中华人民共和国 is six times
 * as wide as a single character) and tells the session, so a page never holds
 * more than fits and paging steps by the same amount. Clamped to
 * [1, IME_SESSION_CAND_PAGE_SIZE]; the compile-time value stays the maximum.
 */
void ime_session_set_page_size(size_t n);

/** Current page size (the value set above, or the compile-time default). */
size_t ime_session_page_size(void);

size_t ime_session_candidate_count(void);

/** Candidate text is UTF-8 and NUL terminated; NULL when out of range. */
const char *ime_session_candidate(size_t index);

/** Index of the highlighted candidate within the whole list. */
size_t ime_session_selected(void);

/** Highlight candidate @p index (absolute index in the whole list). */
ime_change_t ime_session_select(size_t index);

/** Page the candidate list; returns false when already at the boundary. */
bool ime_session_page_next(void);
bool ime_session_page_prev(void);

/** First candidate index of the current page. */
size_t ime_session_page_start(void);

/** Candidates per page. */
size_t ime_session_page_size(void);

/** True when the candidate list has a page after / before the current one. */
bool ime_session_has_next_page(void);
bool ime_session_has_prev_page(void);

/* ------------------------------------------------------------------- state */

/** Current pinyin buffer (UTF-8, may be empty). */
const char *ime_session_pinyin(void);

/**
 * The composition text with the syllable boundaries marked, as the IME displays
 * it: "lawanle" becomes "la'wan'le".
 *
 * A separator is inserted where the engine splits syllables rather than echoed
 * from the buffer, so a separator the user typed by hand is not doubled. Returns
 * the raw buffer unchanged when the engine cannot segment it (English mode, an
 * incomplete syllable, ...).
 *
 * @param out      destination, NUL terminated
 * @param out_size capacity in bytes
 */
void ime_session_pinyin_display(char *out, size_t out_size);

/** Number of fixed characters the engine reports for the current search. */
size_t ime_session_fixed_len(void);

ime_mode_t ime_session_mode(void);
void ime_session_set_mode(ime_mode_t mode);

ime_lang_t ime_session_lang(void);
void ime_session_set_lang(ime_lang_t lang);

ime_shift_t ime_session_shift(void);
void ime_session_set_shift(ime_shift_t shift);

/** Consume a one-shot shift, returning true when the next letter is uppercase. */
bool ime_session_take_one_shot_shift(void);

/* ------------------------------------------------------------- 9-key / T9 */

/** Append a digit (2-9) to the 9-key buffer. */
ime_change_t ime_session_t9_push_digit(char digit);

/** Number of pinyin strings the T9 buffer can currently be split into. */
size_t ime_session_t9_pinyin_count(void);

/** Pinyin string @p index of the T9 candidates (UTF-8, NUL terminated). */
const char *ime_session_t9_pinyin(size_t index);

/** Index of the highlighted T9 pinyin string. */
size_t ime_session_t9_selected(void);

/** Highlight T9 pinyin string @p index and re-search with it. */
ime_change_t ime_session_t9_select(size_t index);

/** Number of digits in the T9 buffer. */
size_t ime_session_t9_len(void);

#ifdef __cplusplus
}
#endif
