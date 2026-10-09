/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Candidate bar: the floating pinyin chip, the row of Chinese candidates and
 * the two paging arrows. The 9-key pinyin row (the row of possible spellings
 * for a digit buffer) lives here too because it shares the candidate styling.
 */

#include "lv_pinyin_ime_internal.h"

#include <string.h>

#include "data/ime_font.h"

static void cand_click_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(btn);

    ime_session_commit_candidate(index);
    /* A candidate tap commits text, so it has to drain the session buffer just
     * like a key press does. Without this the text only reaches the text area
     * when some later key press flushes it. */
    ime_ui_commit_pending(ctx);
    ime_ui_refresh_all(ctx);
}

static void page_click_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    intptr_t dir = (intptr_t)lv_obj_get_user_data(btn);
    bool moved = (dir > 0) ? ime_session_page_next() : ime_session_page_prev();
    if (moved) {
        ime_ui_refresh_candidates(ctx);
    }
}

static void t9_click_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(btn);
    ime_session_t9_select(index);
    /* Selecting a T9 pinyin can commit a single character word. */
    ime_ui_commit_pending(ctx);
    ime_ui_refresh_all(ctx);
}

/** Build one paging arrow of the candidate bar. */
static lv_obj_t *make_pager(lv_obj_t *parent, lv_pinyin_ime_ctx_t *ctx, const char *caption,
                            intptr_t direction)
{
    lv_obj_t *btn = lv_button_create(parent);
    /* Style first, then size: ime_style_apply_key() calls
     * lv_obj_remove_style_all(), which clears local properties too. */
    ime_style_apply_key(btn, true);
    lv_obj_set_size(btn, IME_UI_ROW_HEIGHT_PX, LV_PCT(100));
    lv_obj_set_user_data(btn, (void *)direction);
    lv_obj_add_event_cb(btn, page_click_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, caption);
    lv_obj_center(label);
    return btn;
}

void ime_ui_build_candidates(lv_pinyin_ime_ctx_t *ctx)
{
    ctx->cand_bar = lv_obj_create(ctx->obj);
    ime_style_apply_bar(ctx->cand_bar);
    lv_obj_set_width(ctx->cand_bar, LV_PCT(100));
    lv_obj_set_height(ctx->cand_bar, IME_UI_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(ctx->cand_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->cand_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctx->cand_bar, 2, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->cand_bar, LV_OBJ_FLAG_SCROLLABLE);

    ctx->page_prev = make_pager(ctx->cand_bar, ctx, "<", -1);

    /*
     * Pinyin chip: the syllables being composed ("ni'hao'ma"), shown at the left
     * of the candidate bar and hidden whenever the buffer is empty (the flex
     * layout skips hidden children, so an empty buffer costs no width at all).
     *
     * ★ It is a normal child of the *bar*, not a floating label of the widget.
     *   Two reasons, both learned the hard way:
     *   1) the bar has a fixed height, so the chip can never change the widget's
     *      height - a chip that hangs above the bar needs reserved space, and that
     *      space shows up as an empty grey band across the top of the keyboard;
     *   2) LV_OBJ_FLAG_IGNORE_LAYOUT only keeps a child out of the *layout*, NOT
     *      out of calc_content_height() (that one skips HIDDEN and FLOATING
     *      only). A positioned-outside child of an LV_SIZE_CONTENT parent
     *      therefore feeds its own position back into the parent's size, and
     *      lv_obj_update_layout()'s `while(scr->scr_layout_inv)` has no iteration
     *      limit - the widget can end up re-laying out forever and starve the
     *      task watchdog.
     */
    ctx->chip = lv_label_create(ctx->cand_bar);
    lv_label_set_text(ctx->chip, "");
    ime_style_apply_chip(ctx->chip);
    ime_style_apply_font(ctx->chip, ime_font_small());
    lv_obj_remove_flag(ctx->chip, LV_OBJ_FLAG_CLICKABLE);
    /* Cap it: a long buffer ("zhonghuarenmingongheguo") would otherwise eat the
     * room the candidates need. Truncation with "..." is fine here - the whole
     * point of the chip is showing roughly what is being composed. */
    lv_label_set_long_mode(ctx->chip, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(ctx->chip, LV_PCT(45), LV_PART_MAIN);
    lv_obj_add_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);

    ctx->cand_row = lv_obj_create(ctx->cand_bar);
    lv_obj_remove_style_all(ctx->cand_row);
    lv_obj_set_flex_grow(ctx->cand_row, 1);
    lv_obj_set_height(ctx->cand_row, LV_PCT(100));
    lv_obj_set_flex_flow(ctx->cand_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->cand_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctx->cand_row, 2, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->cand_row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < IME_UI_CAND_MAX; i++) {
        lv_obj_t *btn = lv_button_create(ctx->cand_row);
        /* Style first, then size (see make_pager). */
        ime_style_apply_candidate(btn, false);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, cand_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_center(label);
        ime_style_apply_font(label, ime_font_big());
        ctx->cand_btns[i] = btn;
        ctx->cand_labels[i] = label;
    }

    ctx->page_next = make_pager(ctx->cand_bar, ctx, ">", 1);

    /* 9-key pinyin row. */
    ctx->t9_row = lv_obj_create(ctx->obj);
    lv_obj_remove_style_all(ctx->t9_row);
    lv_obj_set_width(ctx->t9_row, LV_PCT(100));
    lv_obj_set_height(ctx->t9_row, IME_UI_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(ctx->t9_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->t9_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctx->t9_row, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->t9_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->t9_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctx->t9_row, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < IME_UI_T9_PINYIN_MAX; i++) {
        lv_obj_t *btn = lv_button_create(ctx->t9_row);
        /* Style first, then size (see make_pager). */
        ime_style_apply_candidate(btn, false);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, t9_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_center(label);
        ime_style_apply_font(label, ime_font_small());
        ctx->t9_btns[i] = btn;
        ctx->t9_labels[i] = label;
    }
}

void ime_ui_refresh_candidates(lv_pinyin_ime_ctx_t *ctx)
{
    const char *pinyin = ime_session_pinyin();
    bool has_input = (pinyin != NULL && pinyin[0] != '\0');

    /*
     * The chip belongs to the 26-key layout only: the 9-key layout shows the
     * syllables in its own pinyin row, and showing both would print the same
     * thing twice.
     */
    if (has_input && ctx->mode == IME_MODE_K26) {
        /* Show the syllables apart: "la'wan'le" rather than "lawanle". */
        ime_session_pinyin_display(ctx->pinyin_text, sizeof(ctx->pinyin_text));
        lv_label_set_text(ctx->chip, ctx->pinyin_text);
        lv_obj_remove_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);
    }

    size_t count = ime_session_candidate_count();
    size_t page_start = ime_session_page_start();
    size_t selected = ime_session_selected();

    for (size_t i = 0; i < IME_UI_CAND_MAX; i++) {
        const char *text = (i < count) ? ime_session_candidate(page_start + i) : NULL;

        if (text == NULL) {
            lv_obj_add_flag(ctx->cand_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_label_set_text(ctx->cand_labels[i], text);
        lv_obj_remove_flag(ctx->cand_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(ctx->cand_btns[i], (void *)(uintptr_t)(page_start + i));
        ime_style_apply_candidate(ctx->cand_btns[i], page_start + i == selected);
    }

    lv_obj_set_state(ctx->page_prev, LV_STATE_DISABLED, !ime_session_has_prev_page());
    lv_obj_set_state(ctx->page_next, LV_STATE_DISABLED, !ime_session_has_next_page());
}

void ime_ui_refresh_t9(lv_pinyin_ime_ctx_t *ctx)
{
    bool k9 = (ctx->mode == IME_MODE_K9);
    size_t count = k9 ? ime_session_t9_pinyin_count() : 0;
    size_t selected = ime_session_t9_selected();

    /*
     * The row only exists when it has something to show. It is one row tall and
     * transparent, so an empty one is just a band of keyboard background in the
     * middle of the layout - which is exactly what the 9-key layout used to show
     * before the first digit was typed.
     *
     * Showing/hiding it changes the row count, so the fit-to-height maths has to
     * be redone (that is what keeps the widget from growing past its target).
     */
    const bool want = k9 && ctx->t9_row_visible && count > 0;
    const bool shown = !lv_obj_has_flag(ctx->t9_row, LV_OBJ_FLAG_HIDDEN);
    if (want != shown) {
        if (want) {
            lv_obj_remove_flag(ctx->t9_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->t9_row, LV_OBJ_FLAG_HIDDEN);
        }
        ime_ui_apply_sizes(ctx);
    }
    if (!want) {
        return;
    }

    for (size_t i = 0; i < IME_UI_T9_PINYIN_MAX; i++) {
        if (i < count) {
            const char *py = ime_session_t9_pinyin(i);
            lv_label_set_text(ctx->t9_labels[i], py != NULL ? py : "");
            lv_obj_remove_flag(ctx->t9_btns[i], LV_OBJ_FLAG_HIDDEN);
            ime_style_apply_candidate(ctx->t9_btns[i], i == selected);
        } else {
            lv_obj_add_flag(ctx->t9_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
