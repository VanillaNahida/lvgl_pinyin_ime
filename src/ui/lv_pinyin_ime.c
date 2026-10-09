/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public API of the IME widget plus the key dispatch table.
 *
 * The root object owns:
 *   candidate bar (pinyin chip + candidates + paging arrows)
 *   9-key pinyin row (hidden unless 选拼音 is on and the mode is K9)
 *   key rows for the active keyboard / panel
 *
 * Every visible string comes from ime_session, so the widget itself never
 * touches libgooglepinyin.
 */

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"

#include "lv_pinyin_ime_internal.h"

#include <string.h>

#include "core/ime_log.h"
#include "core/ime_session.h"
#include "data/ime_font.h"

static const char *TAG = "lv_pinyin_ime";

int IME_UI_UNIT_PX = 28;
int IME_UI_ROW_HEIGHT_PX = IME_UI_ROW_HEIGHT_DEFAULT;
int IME_UI_FIT_HEIGHT_PX = 0;

static bool s_events_registered;
static lv_event_code_t s_evt_ready;
static lv_event_code_t s_evt_cand_changed;

/* --------------------------------------------------------------- public API */

lv_event_code_t lv_pinyin_ime_event_ready(void)
{
    if (!s_events_registered) {
        s_evt_ready = lv_event_register_id();
        s_evt_cand_changed = lv_event_register_id();
        s_events_registered = true;
    }
    return s_evt_ready;
}

lv_event_code_t lv_pinyin_ime_event_cand_changed(void)
{
    if (!s_events_registered) {
        (void)lv_pinyin_ime_event_ready();
    }
    return s_evt_cand_changed;
}

lv_pinyin_ime_ctx_t *ime_ctx_get(lv_obj_t *obj)
{
    return (obj != NULL) ? lv_obj_get_user_data(obj) : NULL;
}

void ime_root_delete_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx != NULL) {
        lv_free(ctx);
    }
}

/**
 * Derive the layout unit from the parent width so the keys fill the screen.
 *
 * A row is IME_UI_ROW_UNITS units plus a IME_UI_KEY_GAP between each of its keys,
 * so the unit has to leave that many gaps of room. The constants live in
 * lv_pinyin_ime_internal.h next to the key placement that uses them.
 */
static void update_unit_px(lv_obj_t *parent)
{
    lv_coord_t width = (parent != NULL) ? lv_obj_get_width(parent) : 0;
    if (width <= 0) {
        width = 320;
    }

    /* The widget's own padding, plus the gap between each pair of keys. */
    int usable = (int)width - 2 * IME_UI_PAD - (IME_UI_ROW_UNITS - 1) * IME_UI_KEY_GAP;
    int unit = usable / IME_UI_ROW_UNITS;
    if (unit < 12) {
        unit = 12;
    }
    IME_UI_UNIT_PX = unit;
}

/* Clamp range for lv_pinyin_ime_set_row_height(): below 16 px the glyphs no
 * longer fit, above 256 px one row would fill a small screen on its own. */
#define IME_UI_ROW_HEIGHT_MIN 16
#define IME_UI_ROW_HEIGHT_MAX 256

void lv_pinyin_ime_set_row_height(int px)
{
    if (px <= 0) {
        IME_UI_ROW_HEIGHT_PX = IME_UI_ROW_HEIGHT_DEFAULT;
        return;
    }
    if (px < IME_UI_ROW_HEIGHT_MIN) {
        px = IME_UI_ROW_HEIGHT_MIN;
    } else if (px > IME_UI_ROW_HEIGHT_MAX) {
        px = IME_UI_ROW_HEIGHT_MAX;
    }
    IME_UI_ROW_HEIGHT_PX = px;
}

/* Clamp range for lv_pinyin_ime_set_fit_height(). */
#define IME_UI_FIT_HEIGHT_MIN 96
#define IME_UI_FIT_HEIGHT_MAX 720

void lv_pinyin_ime_set_fit_height(int px)
{
    if (px <= 0) {
        IME_UI_FIT_HEIGHT_PX = 0;
        return;
    }
    if (px < IME_UI_FIT_HEIGHT_MIN) {
        px = IME_UI_FIT_HEIGHT_MIN;
    } else if (px > IME_UI_FIT_HEIGHT_MAX) {
        px = IME_UI_FIT_HEIGHT_MAX;
    }
    IME_UI_FIT_HEIGHT_PX = px;
}

/*
 * Fit sizing: derive the row height from the target total height and the
 * *visible* rows, then apply it to the candidate bar, the 9-key pinyin row and
 * every key row.
 *
 * The root is a flex column: pad_all 2 + 2 (top/bottom), pad_row 3 between its
 * children (candidate bar, 9-key pinyin row when visible, key rows container).
 * The key rows container is a flex column too: pad_row 3 between the key rows.
 * Hidden children are skipped by the flex layout, so "visible rows" is exactly
 * what the layout measures:
 *
 *   rows_total = 1 (cand bar) + (K9 && 选拼音 ? 1 : 0) + key_rows
 *   fixed      = 4 + 3*(root_children - 1) + 3*(key_rows - 1)
 *   row_height = (fit - fixed) / rows_total
 *
 * Called at create and after every rebuild, so mode / panel / language / T9
 * switches keep the keyboard filling the same target height. The keys are sized
 * by flex grow from the parent width, so scaling the row height scales the whole
 * keyboard in both dimensions.
 */
void ime_ui_apply_sizes(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->kb_rows == NULL || IME_UI_FIT_HEIGHT_PX <= 0) {
        return;
    }
    const int fit = IME_UI_FIT_HEIGHT_PX;

    const uint32_t key_rows = lv_obj_get_child_count(ctx->kb_rows);
    /* The 9-key pinyin row is counted only while it is actually on screen (it is
     * hidden when empty, see ime_ui_refresh_t9): the hidden flag is the single
     * source of truth, so a row that is not drawn never takes height away from
     * the keys. */
    const bool t9_visible = (ctx->t9_row != NULL) &&
                            !lv_obj_has_flag(ctx->t9_row, LV_OBJ_FLAG_HIDDEN);
    const uint32_t rows_total = 1u + (t9_visible ? 1u : 0u) + key_rows;
    if (rows_total == 0) {
        return;
    }
    const uint32_t root_children = 1u + (t9_visible ? 1u : 0u) + 1u; /* cand + t9? + kb_rows */
    const uint32_t fixed = 2u * IME_UI_PAD            /* root pad_all top+bottom */
                         + 3u * (root_children - 1u)   /* root pad_row gaps */
                         + 3u * (key_rows > 0 ? key_rows - 1u : 0u); /* kb_rows pad_row gaps */

    int rh = (fit - (int)fixed) / (int)rows_total;
    if (rh < IME_UI_ROW_HEIGHT_MIN) {
        rh = IME_UI_ROW_HEIGHT_MIN;
    } else if (rh > IME_UI_ROW_HEIGHT_MAX) {
        rh = IME_UI_ROW_HEIGHT_MAX;
    }
    IME_UI_ROW_HEIGHT_PX = rh;   /* future rows (rebuilds) start from the derived height */

    if (ctx->cand_bar != NULL) {
        lv_obj_set_height(ctx->cand_bar, rh);
    }
    if (ctx->t9_row != NULL) {
        lv_obj_set_height(ctx->t9_row, rh);
    }
    for (uint32_t r = 0; r < key_rows; r++) {
        lv_obj_t *row = lv_obj_get_child(ctx->kb_rows, r);
        if (row != NULL) {
            lv_obj_set_height(row, rh);
        }
    }
}

void lv_pinyin_ime_set_fonts(const lv_font_t *big, const lv_font_t *small)
{
    /*
     * The fonts live in the font subsystem, so this is all it takes: every widget
     * built afterwards picks them up from ime_font_big() / ime_font_small().
     * (Call it before lv_pinyin_ime_create(); see the header.)
     */
    ime_font_set_override(big, small);
}

lv_obj_t *lv_pinyin_ime_create(lv_obj_t *parent)
{
    if (!ime_session_ready() && !ime_session_init()) {
        IME_LOGE(TAG, "engine unavailable, the keyboard was not created");
        return NULL;
    }

    /* A missing font is not fatal: the default LVGL font is used instead. */
    ime_font_init();

    lv_pinyin_ime_ctx_t *ctx = lv_malloc_zeroed(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    update_unit_px(parent);

    ctx->obj = lv_obj_create(parent);
    ime_style_apply_root(ctx->obj);
    /*
     * 拼音浮窗画在候选栏上方（内容区之外），先把它自己和祖先的"允许溢出"配好，
     * 否则浮窗会被裁掉、看不见（见 ime_ui_chip_prepare）。
     */
    ime_ui_chip_prepare(ctx->obj);
    lv_obj_set_size(ctx->obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_SCROLLABLE);
    /* Only the keys swallow taps; the gaps have to fall through. */
    lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ctx->obj, ctx);
    lv_obj_add_event_cb(ctx->obj, ime_root_delete_cb, LV_EVENT_DELETE, ctx);

    ctx->mode = ime_session_mode();
    ctx->lang = ime_session_lang();
    ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
    ctx->t9_row_visible = true;

    ime_ui_build_candidates(ctx);

    ctx->kb_rows = lv_obj_create(ctx->obj);
    lv_obj_remove_style_all(ctx->kb_rows);
    lv_obj_set_width(ctx->kb_rows, LV_PCT(100));
    lv_obj_set_height(ctx->kb_rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->kb_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ctx->kb_rows, 3, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->kb_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ctx->kb_rows, LV_OBJ_FLAG_CLICKABLE);

    ime_ui_rebuild_keyboard(ctx);
    ime_ui_apply_sizes(ctx);     /* fit 模式：把行高按目标总高分摊到每一行 */
    ime_ui_refresh_all(ctx);
    return ctx->obj;
}

void lv_pinyin_ime_attach(lv_obj_t *ime, lv_obj_t *ta)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL) {
        return;
    }
    ctx->ta = ta;

    /*
     * Text that reaches the text area is committed Chinese, so it needs the CJK
     * font: LV_FONT_DEFAULT (Montserrat) has no Hanzi and would draw boxes. The
     * font is only applied while the target still uses the default, so an
     * application that picked its own font keeps it.
     *
     * The placeholder is drawn with LV_PART_TEXTAREA_PLACEHOLDER, which does not
     * inherit from LV_PART_MAIN on this widget, so it has to be set as well or
     * the hint text keeps using the default font.
     */
    if (ta != NULL && ime_font_big() != NULL &&
        lv_obj_get_style_text_font(ta, LV_PART_MAIN) == LV_FONT_DEFAULT) {
        lv_obj_set_style_text_font(ta, ime_font_big(), LV_PART_MAIN);
        lv_obj_set_style_text_font(ta, ime_font_big(), LV_PART_TEXTAREA_PLACEHOLDER);
    }
}

void lv_pinyin_ime_set_mode(lv_obj_t *ime, lv_pinyin_ime_mode_t mode)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL || mode >= LV_PINYIN_IME_MODE_LAST) {
        return;
    }
    ctx->mode = (ime_mode_t)mode;
    ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
    ctx->panel_alt = false;
    ime_session_set_mode((ime_mode_t)mode);
    ime_ui_rebuild_keyboard(ctx);
    ime_ui_refresh_all(ctx);
}

lv_pinyin_ime_mode_t lv_pinyin_ime_get_mode(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    return ctx != NULL ? (lv_pinyin_ime_mode_t)ctx->mode : LV_PINYIN_IME_MODE_K26;
}

void lv_pinyin_ime_set_lang(lv_obj_t *ime, lv_pinyin_ime_lang_t lang)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL || lang >= LV_PINYIN_IME_LANG_LAST) {
        return;
    }
    ime_ui_set_lang(ctx, (ime_lang_t)lang);
    ime_ui_refresh_all(ctx);
}

lv_pinyin_ime_lang_t lv_pinyin_ime_get_lang(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    return ctx != NULL ? (lv_pinyin_ime_lang_t)ctx->lang : LV_PINYIN_IME_LANG_CN;
}

void lv_pinyin_ime_set_panel(lv_obj_t *ime, lv_pinyin_ime_panel_t panel)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL || panel >= LV_PINYIN_IME_PANEL_LAST) {
        return;
    }
    ctx->panel = panel;
    ctx->panel_alt = false;
    ime_ui_rebuild_keyboard(ctx);
    ime_ui_refresh_all(ctx);
}

lv_pinyin_ime_panel_t lv_pinyin_ime_get_panel(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    return ctx != NULL ? ctx->panel : LV_PINYIN_IME_PANEL_MAIN;
}

void lv_pinyin_ime_reset(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    ime_session_reset();
    if (ctx != NULL) {
        ime_ui_refresh_all(ctx);
    }
}

void lv_pinyin_ime_show(lv_obj_t *ime)
{
    lv_obj_remove_flag(ime, LV_OBJ_FLAG_HIDDEN);
}

void lv_pinyin_ime_hide(lv_obj_t *ime)
{
    lv_obj_add_flag(ime, LV_OBJ_FLAG_HIDDEN);
}

void lv_pinyin_ime_dump(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL) {
        IME_LOGE(TAG, "dump: not an IME object (%p)", (void *)ime);
        return;
    }

    lv_obj_update_layout(ctx->obj);

    lv_area_t area;
    lv_obj_get_coords(ctx->obj, &area);
    IME_LOGI(TAG, "IME coords (%d,%d)-(%d,%d) %dx%d hidden=%d",
             (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
             (int)lv_area_get_width(&area), (int)lv_area_get_height(&area),
             (int)lv_obj_has_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN));
    IME_LOGI(TAG, "mode=%d lang=%d panel=%d unit=%dpx engine=%d font=%d/%d",
             (int)ctx->mode, (int)ctx->lang, (int)ctx->panel, IME_UI_UNIT_PX,
             (int)ime_session_ready(), ime_font_big() != NULL, ime_font_small() != NULL);

    /* The key rows: objects and their first row's buttons say whether the
     * keyboard was actually built and how big it came out. */
    uint32_t rows = lv_obj_get_child_count(ctx->kb_rows);
    IME_LOGI(TAG, "key rows: %u", (unsigned)rows);
    for (uint32_t r = 0; r < rows; r++) {
        lv_obj_t *row = lv_obj_get_child(ctx->kb_rows, r);
        lv_obj_get_coords(row, &area);
        uint32_t keys = lv_obj_get_child_count(row);
        uint32_t visible = 0;
        for (uint32_t k = 0; k < keys; k++) {
            if (!lv_obj_has_flag(lv_obj_get_child(row, k), LV_OBJ_FLAG_HIDDEN)) {
                visible++;
            }
        }
        IME_LOGI(TAG, "  row %u: %u keys (%u visible) at y=%d size %dx%d", (unsigned)r,
                 (unsigned)keys, (unsigned)visible, (int)area.y1,
                 (int)lv_area_get_width(&area), (int)lv_area_get_height(&area));

        /* The first key's width and the row's left edge say whether the columns
         * line up: every row should start at the same x with the same key width. */
        if (keys > 0) {
            lv_area_t key_area;
            lv_obj_get_coords(lv_obj_get_child(row, 0), &key_area);
            IME_LOGI(TAG, "    first key x=%d w=%d", (int)key_area.x1,
                     (int)lv_area_get_width(&key_area));
        }
    }

    IME_LOGI(TAG, "candidates: %u shown, total %u, pinyin \"%s\"",
             (unsigned)ime_session_candidate_count(),
             (unsigned)ime_session_page_start(), ime_session_pinyin());
}

/* ---------------------------------------------------------------- dispatch */

void ime_ui_refresh_all(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ime_ui_refresh_candidates(ctx);
    ime_ui_refresh_t9(ctx);
    lv_obj_send_event(ctx->obj, lv_pinyin_ime_event_cand_changed(), NULL);
}

char ime_ui_digit_for_group(const char *caption)
{
    if (caption == NULL || caption[0] == '\0') {
        return '0';
    }
    switch (caption[0]) {
    case 'A':
        return '2';
    case 'D':
        return '3';
    case 'G':
        return '4';
    case 'J':
        return '5';
    case 'M':
        return '6';
    case 'P':
        return '7';
    case 'T':
        return '8';
    case 'W':
        return '9';
    default:
        return '0';
    }
}

const char *ime_ui_fullwidth(const lv_pinyin_ime_ctx_t *ctx, const char *label)
{
    if (ctx == NULL || ctx->lang == IME_LANG_EN || label == NULL) {
        return label;
    }
    if (strcmp(label, "-") == 0) {
        return "－";
    }
    if (strcmp(label, ".") == 0) {
        return "。";
    }
    if (strcmp(label, ",") == 0) {
        return "，";
    }
    if (strcmp(label, ":") == 0) {
        return "：";
    }
    return label;
}

void ime_ui_cycle_shift(lv_pinyin_ime_ctx_t *ctx)
{
    (void)ctx;
    switch (ime_session_shift()) {
    case IME_SHIFT_OFF:
        ime_session_set_shift(IME_SHIFT_ONCE);
        break;
    case IME_SHIFT_ONCE:
        ime_session_set_shift(IME_SHIFT_LOCK);
        break;
    default:
        ime_session_set_shift(IME_SHIFT_OFF);
        break;
    }
}

void ime_ui_commit_pending(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    char text[IME_ENGINE_MAX_TEXT_LEN * 3 + 1];
    while (ime_session_take_commit(text, sizeof(text))) {
        if (ctx->ta != NULL && text[0] != '\0') {
            lv_textarea_add_text(ctx->ta, text);
        }
    }
}

/* ------------------------------------------------------- backspace behaviour */

void ime_ui_backspace(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /*
     * The session removes pinyin first and reports NONE once there is nothing
     * left to remove; only then does the character in the text area go. Both the
     * tap and the hold-to-repeat path come through here, so holding the key
     * deletes one character at a time exactly like tapping it.
     */
    if (ime_session_backspace() == IME_CHANGE_NONE && ctx->ta != NULL) {
        lv_textarea_delete_char(ctx->ta);
    }
}

static void bksp_hint_ensure(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx->bksp_hint != NULL) {
        return;
    }
    ctx->bksp_hint = lv_label_create(ctx->obj);
    lv_label_set_text(ctx->bksp_hint, IME_UI_BKSP_HINT_TEXT);
    ime_style_apply_chip(ctx->bksp_hint);
    ime_style_apply_font(ctx->bksp_hint, ime_font_small());
    lv_obj_add_flag(ctx->bksp_hint, LV_OBJ_FLAG_HIDDEN);
    /*
     * FLOATING, not IGNORE_LAYOUT: the root is a flex column whose height is
     * LV_SIZE_CONTENT, and calc_content_height() skips HIDDEN and FLOATING
     * children only - an IGNORE_LAYOUT child still counts towards the parent's
     * content height (see the pinyin chip in lv_pinyin_ime_cand.c for what that
     * feedback loop does to lv_obj_update_layout()). The hint is an overlay: it
     * must not take part in the layout *or* in the size.
     */
    lv_obj_add_flag(ctx->bksp_hint, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(ctx->bksp_hint, LV_OBJ_FLAG_CLICKABLE);
}

void ime_ui_bksp_hold_begin(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x)
{
    if (ctx == NULL) {
        return;
    }
    ctx->bksp_press_x = finger_x;
    ctx->bksp_hold = false;
    ctx->bksp_cleared = false;
    /* A new press starts a new decision about the release-time CLICKED. */
    ctx->bksp_consumed = false;
}

void ime_ui_bksp_hold_arm(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->bksp_hold = true;
    ctx->bksp_cleared = false;
    bksp_hint_ensure(ctx);

    /*
     * The pill sits in the top right corner of the widget, which is directly
     * above the backspace key (it ends the first key row), and it grows to the
     * left. Sliding the finger left by the width of the pill therefore puts it at
     * the far end of the pill - the position the hint points at.
     */
    lv_obj_remove_flag(ctx->bksp_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ctx->bksp_hint, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_move_foreground(ctx->bksp_hint);
    lv_obj_update_layout(ctx->bksp_hint);

    int32_t need = (int32_t)lv_obj_get_width(ctx->bksp_hint) + 8;
    if (need < IME_UI_BKSP_CLEAR_MIN) {
        need = IME_UI_BKSP_CLEAR_MIN;
    } else if (need > IME_UI_BKSP_CLEAR_MAX) {
        need = IME_UI_BKSP_CLEAR_MAX;
    }
    ctx->bksp_drag_px = need;
}

void ime_ui_bksp_hold_move(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x)
{
    if (ctx == NULL || !ctx->bksp_hold || ctx->bksp_cleared) {
        return;
    }
    if (ctx->bksp_press_x - finger_x < ctx->bksp_drag_px) {
        return;                 /* not far enough left yet */
    }

    ctx->bksp_cleared = true;
    /*
     * Clear the input box: the committed text and the composition buffer both
     * belong to it, and leaving half typed pinyin behind would keep the
     * candidate bar busy.
     */
    ime_session_reset();
    if (ctx->ta != NULL) {
        lv_textarea_set_text(ctx->ta, "");
    }
    if (ctx->bksp_hint != NULL) {
        lv_obj_add_flag(ctx->bksp_hint, LV_OBJ_FLAG_HIDDEN);
    }
    ime_ui_refresh_all(ctx);
}

void ime_ui_bksp_hold_end(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->bksp_hold = false;
    ctx->bksp_cleared = false;
    if (ctx->bksp_hint != NULL) {
        lv_obj_add_flag(ctx->bksp_hint, LV_OBJ_FLAG_HIDDEN);
    }
    /*
     * bksp_consumed is deliberately kept: LVGL sends CLICKED right after
     * RELEASED, and that trailing event has to stay swallowed when the hold
     * already deleted something.
     */
}

bool ime_ui_bksp_take_consumed(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->bksp_consumed) {
        return false;
    }
    ctx->bksp_consumed = false;
    return true;
}

void ime_ui_set_lang(lv_pinyin_ime_ctx_t *ctx, ime_lang_t lang)
{
    if (ctx == NULL) {
        return;
    }
    ctx->lang = lang;
    ime_session_set_lang(lang);

    /*
     * Reset the shift state on every language change.
     *
     * The caps-lock key does nothing while Chinese is active (the letters are
     * drawn upper case regardless), so a locked state left over from English
     * could neither be seen nor cleared there - and it would suddenly apply
     * again the next time English is selected. Dropping it keeps each language
     * starting from the same, predictable state.
     */
    ime_session_set_shift(IME_SHIFT_OFF);

    ime_ui_rebuild_keyboard(ctx);
}

void ime_ui_handle_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key)
{
    if (ctx == NULL || key == NULL) {
        return;
    }

    switch (key->action) {
    case IME_KEY_LETTER:
        /*
         * Always hand the lower-case letter over: the session owns the shift
         * state machine and decides the case (English only), so the UI and the
         * committed text cannot disagree about it.
         *
         * ★ However, the session may *spend* an English one-shot shift on this
         *   very letter - and the key captions are drawn from the shift state. If
         *   the captions are not redrawn here, the keys stay upper case after the
         *   state has already gone back to off, i.e. the keyboard looks locked
         *   although the next letter typed is lower case.
         */
        {
            const ime_shift_t before = ime_session_shift();
            ime_session_push_letter(key->label[0]);
            if (ime_session_shift() != before) {
                ime_ui_rebuild_keyboard(ctx);
            }
        }
        break;

    case IME_KEY_DIGIT:
        ime_session_t9_push_digit(ime_ui_digit_for_group(key->label));
        break;

    case IME_KEY_TEXT:
        ime_session_commit_literal(key->text != NULL ? key->text : key->label);
        break;

    case IME_KEY_FULLWIDTH:
        ime_session_commit_literal(ime_ui_fullwidth(ctx, key->label));
        break;

    case IME_KEY_BACKSPACE:
        ime_ui_backspace(ctx);
        break;

    case IME_KEY_ENTER:
        if (ctx->ta != NULL) {
            lv_textarea_add_char(ctx->ta, '\n');
        }
        break;

    case IME_KEY_SPACE:
        /*
         * While composing, space takes the highlighted candidate - the usual
         * Chinese IME behaviour, and much faster than reaching for the number
         * row. commit_current() also handles a half typed syllable by passing
         * the raw letters through, so nothing the user typed is lost. With no
         * composition at all it inserts a real space.
         */
        if (ime_session_has_input()) {
            ime_session_commit_current();
        } else {
            ime_session_commit_literal(" ");
        }
        break;

    case IME_KEY_SEPARATOR:
        ime_session_push_separator();
        break;

    case IME_KEY_CONFIRM:
        if (!ime_session_has_input()) {
            /* Nothing buffered: take the keyboard down and tell the app. */
            lv_obj_send_event(ctx->obj, lv_pinyin_ime_event_ready(), NULL);
            return;
        }
        ime_session_commit_current();
        break;

    case IME_KEY_SHIFT:
        /*
         * The caps-lock key only means something in English: the Chinese
         * keyboard always draws upper-case letters and the state has no effect
         * there, so pressing it must not do anything either (a silent state
         * change would come back to life after switching to English).
         * Long press - the language switch - is handled in the key callbacks
         * and stays available in both languages.
         */
        if (ctx->lang != IME_LANG_EN) {
            break;
        }
        ime_ui_cycle_shift(ctx);
        ime_ui_rebuild_keyboard(ctx);
        break;

    case IME_KEY_SWITCH_KB:
        ctx->mode = (ctx->mode == IME_MODE_K26) ? IME_MODE_K9 : IME_MODE_K26;
        ime_session_set_mode(ctx->mode);
        ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
        ctx->panel_alt = false;
        ime_ui_rebuild_keyboard(ctx);
        break;

    case IME_KEY_LANG:
        ime_ui_set_lang(ctx, (ctx->lang == IME_LANG_CN) ? IME_LANG_EN : IME_LANG_CN);
        ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
        ctx->panel_alt = false;
        ime_ui_rebuild_keyboard(ctx);
        break;

    case IME_KEY_PANEL:
        if (key->text != NULL && strcmp(key->text, "main") == 0) {
            ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
            ctx->panel_alt = false;
        } else if (key->text != NULL && strcmp(key->text, "num") == 0) {
            ctx->panel = LV_PINYIN_IME_PANEL_NUM;
        } else if (key->text != NULL && strcmp(key->text, "all") == 0) {
            ctx->panel = LV_PINYIN_IME_PANEL_ALL_SYM;
        } else if (key->text != NULL && strcmp(key->text, "alt") == 0) {
            ctx->panel_alt = !ctx->panel_alt;
        } else {
            ctx->panel = LV_PINYIN_IME_PANEL_NUM_SYM;
            ctx->panel_alt = false;
        }
        ime_ui_rebuild_keyboard(ctx);
        break;

    case IME_KEY_T9_PINYIN:
        ctx->t9_row_visible = !ctx->t9_row_visible;
        /* 可见行数变了：fit 模式下重新分摊行高，整块仍占目标高度 */
        ime_ui_apply_sizes(ctx);
        break;

    case IME_KEY_NONE:
    default:
        return;
    }

    ime_ui_commit_pending(ctx);
    ime_ui_refresh_all(ctx);
}
