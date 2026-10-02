/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ime_session.h"

#include "../data/ime_dict.h"
#include "ime_log.h"
#include "ime_t9.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ime_session";

#define CAND_MAX IME_SESSION_MAX_CANDIDATES
#define CAND_TEXT_MAX 48

typedef struct {
    char text[CAND_TEXT_MAX];
    uint8_t len;
} ime_candidate_t;

typedef struct {
    bool inited;
    ime_mode_t mode;
    ime_lang_t lang;
    ime_shift_t shift;
    bool one_shot_shift;

    char pinyin[IME_SESSION_MAX_PINYIN_LEN + 1];
    size_t pinyin_len;

    /* The whole candidate list is fetched once per search (bounded by
     * IME_SESSION_MAX_CANDIDATES); paging only moves the window. */
    ime_candidate_t candidates[CAND_MAX];
    size_t cand_total;
    size_t page_start;
    size_t selected;
    size_t page_count;

    char commit[IME_ENGINE_MAX_TEXT_LEN * 3 + 1];
    bool has_commit;
} ime_session_t;

static ime_session_t s_ime;

/* ------------------------------------------------------------------ helpers */

static void window_refresh(void)
{
    size_t remaining = (s_ime.cand_total > s_ime.page_start) ? s_ime.cand_total - s_ime.page_start : 0;
    size_t n = (remaining > IME_SESSION_CAND_PAGE_SIZE) ? IME_SESSION_CAND_PAGE_SIZE : remaining;
    s_ime.page_count = n;
    if (s_ime.selected < s_ime.page_start || s_ime.selected >= s_ime.page_start + n) {
        s_ime.selected = s_ime.page_start;
    }
}

static void refresh_candidates(void)
{
    size_t total = ime_engine_search(s_ime.pinyin, s_ime.pinyin_len);
    if (total > CAND_MAX) {
        total = CAND_MAX;
    }
    s_ime.cand_total = total;
    s_ime.page_start = 0;
    s_ime.selected = 0;

    for (size_t i = 0; i < total; i++) {
        size_t n = ime_engine_candidate(i, s_ime.candidates[i].text, CAND_TEXT_MAX);
        s_ime.candidates[i].len = (uint8_t)n;
    }
    window_refresh();
}

static void load_page(size_t start)
{
    if (start >= s_ime.cand_total) {
        return;
    }
    s_ime.page_start = start;
    s_ime.selected = start;
    window_refresh();
}

static void set_commit(const char *text)
{
    snprintf(s_ime.commit, sizeof(s_ime.commit), "%s", text);
    s_ime.has_commit = true;
}

static void clear_input(void)
{
    s_ime.pinyin[0] = '\0';
    s_ime.pinyin_len = 0;
    s_ime.cand_total = 0;
    s_ime.page_count = 0;
    s_ime.selected = 0;
    s_ime.page_start = 0;
    ime_engine_reset();
    ime_t9_reset();
}

/**
 * Commit the candidate at @p index, keeping the rest of the composition alive.
 *
 * Choosing a candidate fixes it inside the engine (see MatrixSearch::choose) and
 * the engine then holds only the syllables that are still unfixed; that remainder
 * becomes the new pinyin buffer, so a user who picked 拉 out of "lawanle" carries
 * on with "wanle" instead of losing it.
 *
 * Only the part that was actually committed is emitted. For a whole-sentence
 * candidate the committed part is the whole string; for a shorter lemma it is
 * the candidate minus the tail that the engine is still holding, which is
 * exactly "the syllables this candidate used up".
 */
static ime_change_t commit_candidate_keep_composing(size_t index)
{
    const char *before = ime_session_candidate(index);
    if (before == NULL) {
        return IME_CHANGE_NONE;
    }

    ime_engine_choose(index);

    size_t remaining_len = 0;
    const char *remaining = ime_engine_remaining_pinyin(&remaining_len);

    if (remaining != NULL && remaining_len > 0) {
        /*
         * Keep composing the rest: 拉 out of lawanle leaves "wanle".
         *
         * The engine's buffer is copied before being handed to anything else,
         * because refresh_candidates() below re-enters the engine and the
         * remaining-pinyin pointer is only valid until the next search.
         */
        char tail[IME_SESSION_MAX_PINYIN_LEN + 1];
        snprintf(tail, sizeof(tail), "%.*s", (int)IME_SESSION_MAX_PINYIN_LEN, remaining);
        set_commit(before);

        snprintf(s_ime.pinyin, sizeof(s_ime.pinyin), "%s", tail);
        s_ime.pinyin_len = strlen(s_ime.pinyin);
        refresh_candidates();
        /* Both happened, but ime_change_t is a sequential enum rather than a
         * bitmask, so report the one that drives the bigger redraw. */
        return IME_CHANGE_CANDIDATES;
    }

    /* Fully consumed: the whole candidate is the text. */
    set_commit(before);
    clear_input();
    return IME_CHANGE_COMMIT;
}

/* ------------------------------------------------------------- lifecycle */

bool ime_session_init(void)
{
    if (s_ime.inited) {
        return true;
    }

    memset(&s_ime, 0, sizeof(s_ime));
    s_ime.mode = IME_MODE_K26;
    s_ime.lang = IME_LANG_CN;
    s_ime.shift = IME_SHIFT_OFF;

    char sys_path[64];
    if (!ime_dict_prepare(sys_path, sizeof(sys_path))) {
        IME_LOGE(TAG, "system dictionary unavailable");
        return false;
    }

    /* Prove the file itself is readable before blaming the engine: a failure
     * here is a VFS or mapping problem, not a dictionary problem. */
    ime_dict_self_test();

    const char *usr_path = ime_dict_user_path();
    if (!ime_engine_open(sys_path, usr_path)) {
        /* Say how big the file was and what it starts with: a partition that
         * holds something other than a dictionary (or nothing at all) is the
         * usual cause, and the first bytes tell the two apart. */
        FILE *fp = fopen(sys_path, "rb");
        if (fp != NULL) {
            unsigned char head[8] = {0};
            size_t got = fread(head, 1, sizeof(head), fp);
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fclose(fp);
            IME_LOGE(TAG, "ime_engine_open(%s) failed; file is %ld bytes, first %u bytes: "
                          "%02X %02X %02X %02X %02X %02X %02X %02X",
                     sys_path, size, (unsigned)got, head[0], head[1], head[2], head[3],
                     head[4], head[5], head[6], head[7]);
        } else {
            IME_LOGE(TAG, "ime_engine_open(%s) failed and the file cannot be opened", sys_path);
        }
        ime_dict_release();
        return false;
    }

    ime_engine_set_max_lens(IME_SESSION_MAX_PINYIN_LEN, 32);
    ime_t9_init();
    s_ime.inited = true;
    return true;
}

void ime_session_deinit(void)
{
    if (!s_ime.inited) {
        return;
    }
    ime_engine_close();
    ime_dict_release();
    ime_t9_deinit();
    memset(&s_ime, 0, sizeof(s_ime));
}

bool ime_session_ready(void)
{
    return s_ime.inited;
}

void ime_session_reset(void)
{
    clear_input();
}

/* ------------------------------------------------------------------ input */

static ime_change_t push_pinyin_char(char ch)
{
    if (s_ime.pinyin_len >= IME_SESSION_MAX_PINYIN_LEN) {
        return IME_CHANGE_NONE;
    }
    s_ime.pinyin[s_ime.pinyin_len++] = ch;
    s_ime.pinyin[s_ime.pinyin_len] = '\0';
    refresh_candidates();
    return IME_CHANGE_CANDIDATES;
}

ime_change_t ime_session_push_letter(char ch)
{
    if (!s_ime.inited) {
        return IME_CHANGE_NONE;
    }

    bool upper = false;
    if (ch >= 'A' && ch <= 'Z') {
        upper = true;
        ch = (char)(ch - 'A' + 'a');
    }
    if (ch < 'a' || ch > 'z') {
        return IME_CHANGE_NONE;
    }

    if (s_ime.one_shot_shift) {
        upper = true;
        s_ime.one_shot_shift = false;
    }

    if (s_ime.lang == IME_LANG_EN) {
        char text[2] = {(char)(upper ? (ch - 'a' + 'A') : ch), '\0'};
        set_commit(text);
        return IME_CHANGE_COMMIT;
    }

    return push_pinyin_char(ch);
}

ime_change_t ime_session_push_separator(void)
{
    if (!s_ime.inited || s_ime.lang == IME_LANG_EN) {
        return IME_CHANGE_NONE;
    }
    if (s_ime.pinyin_len == 0 || s_ime.pinyin[s_ime.pinyin_len - 1] == '\'') {
        return IME_CHANGE_NONE;
    }
    return push_pinyin_char('\'');
}

ime_change_t ime_session_backspace(void)
{
    if (!s_ime.inited) {
        return IME_CHANGE_NONE;
    }

    if (s_ime.mode == IME_MODE_K9 && ime_t9_len() > 0) {
        ime_t9_backspace();
        if (ime_t9_len() > 0) {
            const char *py = ime_t9_selected_pinyin();
            snprintf(s_ime.pinyin, sizeof(s_ime.pinyin), "%s", py != NULL ? py : "");
            s_ime.pinyin_len = strlen(s_ime.pinyin);
            refresh_candidates();
        } else {
            clear_input();
        }
        return IME_CHANGE_CANDIDATES;
    }

    if (s_ime.pinyin_len > 0) {
        s_ime.pinyin_len--;
        s_ime.pinyin[s_ime.pinyin_len] = '\0';

        if (s_ime.pinyin_len == 0) {
            clear_input();
        } else {
            refresh_candidates();
        }
        return IME_CHANGE_CANDIDATES;
    }

    /* Nothing buffered: the UI deletes one character of the bound text area. */
    return IME_CHANGE_NONE;
}

ime_change_t ime_session_commit_candidate(size_t index)
{
    const char *text = ime_session_candidate(index);
    if (text == NULL) {
        return IME_CHANGE_NONE;
    }
    return commit_candidate_keep_composing(index);
}

ime_change_t ime_session_commit_current(void)
{
    if (!s_ime.inited) {
        return IME_CHANGE_NONE;
    }

    if (s_ime.page_count > 0) {
        return ime_session_commit_candidate(s_ime.selected);
    }

    if (s_ime.pinyin_len > 0) {
        /* No candidate (for example a half typed syllable): pass the letters
         * through so nothing the user typed is lost. */
        set_commit(s_ime.pinyin);
        clear_input();
        return IME_CHANGE_COMMIT;
    }

    return IME_CHANGE_NONE;
}

bool ime_session_take_commit(char *out, size_t out_size)
{
    if (!s_ime.has_commit || out == NULL || out_size == 0) {
        return false;
    }
    snprintf(out, out_size, "%s", s_ime.commit);
    s_ime.has_commit = false;
    s_ime.commit[0] = '\0';
    return true;
}

bool ime_session_has_commit(void)
{
    return s_ime.has_commit;
}

void ime_session_commit_literal(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    /* A literal also ends the current pinyin composition. */
    clear_input();
    set_commit(text);
}

bool ime_session_has_input(void)
{
    return s_ime.pinyin_len > 0 || s_ime.page_count > 0;
}

/* -------------------------------------------------------------- candidates */

size_t ime_session_candidate_count(void)
{
    return s_ime.page_count;
}

const char *ime_session_candidate(size_t index)
{
    if (index >= s_ime.cand_total) {
        return NULL;
    }
    return s_ime.candidates[index].text;
}

size_t ime_session_selected(void)
{
    return s_ime.selected;
}

ime_change_t ime_session_select(size_t index)
{
    if (index >= s_ime.cand_total) {
        return IME_CHANGE_NONE;
    }
    s_ime.selected = index;

    if (index < s_ime.page_start || index >= s_ime.page_start + s_ime.page_count) {
        load_page(index - (index % IME_SESSION_CAND_PAGE_SIZE));
    }
    return IME_CHANGE_CANDIDATES;
}

bool ime_session_page_next(void)
{
    if (!ime_session_has_next_page()) {
        return false;
    }
    load_page(s_ime.page_start + IME_SESSION_CAND_PAGE_SIZE);
    return true;
}

bool ime_session_page_prev(void)
{
    if (!ime_session_has_prev_page()) {
        return false;
    }
    load_page(s_ime.page_start - IME_SESSION_CAND_PAGE_SIZE);
    return true;
}

size_t ime_session_page_start(void)
{
    return s_ime.page_start;
}

size_t ime_session_page_size(void)
{
    return IME_SESSION_CAND_PAGE_SIZE;
}

bool ime_session_has_next_page(void)
{
    return s_ime.cand_total > s_ime.page_start + s_ime.page_count;
}

bool ime_session_has_prev_page(void)
{
    return s_ime.page_start > 0;
}

/* ------------------------------------------------------------------- state */

const char *ime_session_pinyin(void)
{
    return s_ime.pinyin;
}

void ime_session_pinyin_display(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (s_ime.pinyin_len == 0 || s_ime.lang == IME_LANG_EN) {
        return;
    }

    /*
     * Insert the separator at the engine's syllable boundaries. spl_start[i] is
     * the offset of the i-th spelling and there is one extra entry, so the
     * boundaries are simply every element except the first (0) and the last
     * (the total length).
     */
    const uint16_t *starts = NULL;
    size_t count = ime_engine_spl_start_pos(&starts);
    if (starts == NULL || count == 0) {
        snprintf(out, out_size, "%s", s_ime.pinyin);
        return;
    }

    size_t written = 0;
    for (size_t i = 0; i < s_ime.pinyin_len && written + 1 < out_size; i++) {
        bool boundary = false;
        for (size_t s = 1; s < count; s++) {
            if (starts[s] == i) {
                boundary = true;
                break;
            }
        }
        /* Never emit two separators in a row, nor one at the very start: a user
         * typed separator is already in the buffer. */
        if (boundary && written > 0 && out[written - 1] != '\'') {
            out[written++] = '\'';
            if (written + 1 >= out_size) {
                break;
            }
        }
        out[written++] = s_ime.pinyin[i];
    }
    out[written] = '\0';
}

size_t ime_session_fixed_len(void)
{
    return ime_engine_fixed_len();
}

ime_mode_t ime_session_mode(void)
{
    return s_ime.mode;
}

void ime_session_set_mode(ime_mode_t mode)
{
    if (mode >= IME_MODE_LAST || mode == s_ime.mode) {
        return;
    }
    s_ime.mode = mode;
    clear_input();
}

ime_lang_t ime_session_lang(void)
{
    return s_ime.lang;
}

void ime_session_set_lang(ime_lang_t lang)
{
    if (lang >= IME_LANG_LAST || lang == s_ime.lang) {
        return;
    }
    s_ime.lang = lang;
    clear_input();
}

ime_shift_t ime_session_shift(void)
{
    return s_ime.shift;
}

void ime_session_set_shift(ime_shift_t shift)
{
    if (shift >= 3) {
        return;
    }
    s_ime.shift = shift;
    s_ime.one_shot_shift = (shift == IME_SHIFT_ONCE);
}

bool ime_session_take_one_shot_shift(void)
{
    if (!s_ime.one_shot_shift) {
        return false;
    }
    s_ime.one_shot_shift = false;
    s_ime.shift = IME_SHIFT_OFF;
    return true;
}

/* ------------------------------------------------------------- 9-key / T9 */

ime_change_t ime_session_t9_push_digit(char digit)
{
    if (!s_ime.inited || ime_t9_len() >= IME_T9_MAX_DIGITS) {
        return IME_CHANGE_NONE;
    }
    if (!ime_t9_push_digit(digit)) {
        return IME_CHANGE_NONE;
    }

    const char *py = ime_t9_selected_pinyin();
    snprintf(s_ime.pinyin, sizeof(s_ime.pinyin), "%s", py != NULL ? py : "");
    s_ime.pinyin_len = strlen(s_ime.pinyin);
    refresh_candidates();
    return IME_CHANGE_CANDIDATES;
}

size_t ime_session_t9_pinyin_count(void)
{
    return ime_t9_count();
}

const char *ime_session_t9_pinyin(size_t index)
{
    return ime_t9_pinyin(index);
}

size_t ime_session_t9_selected(void)
{
    return ime_t9_selected_index();
}

ime_change_t ime_session_t9_select(size_t index)
{
    if (!ime_t9_select(index)) {
        return IME_CHANGE_NONE;
    }
    const char *py = ime_t9_selected_pinyin();
    snprintf(s_ime.pinyin, sizeof(s_ime.pinyin), "%s", py != NULL ? py : "");
    s_ime.pinyin_len = strlen(s_ime.pinyin);
    refresh_candidates();
    return IME_CHANGE_CANDIDATES;
}

size_t ime_session_t9_len(void)
{
    return ime_t9_len();
}
