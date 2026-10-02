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
    lv_obj_set_size(ctx->obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(ctx->obj, false);
    /* Only the keys swallow taps; the gaps have to fall through. */
    lv_obj_set_clickable(ctx->obj, false);
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
    lv_obj_set_scrollable(ctx->kb_rows, false);
    lv_obj_set_clickable(ctx->kb_rows, false);

    ime_ui_rebuild_keyboard(ctx);
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
    ctx->lang = (ime_lang_t)lang;
    ime_session_set_lang((ime_lang_t)lang);
    ime_ui_rebuild_keyboard(ctx);
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
    lv_obj_set_hidden(ime, false);
}

void lv_pinyin_ime_hide(lv_obj_t *ime)
{
    lv_obj_set_hidden(ime, true);
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

void ime_ui_handle_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key)
{
    if (ctx == NULL || key == NULL) {
        return;
    }

    switch (key->action) {
    case IME_KEY_LETTER: {
        char ch = key->label[0];
        if (ime_session_shift() != IME_SHIFT_OFF) {
            ch = (char)(ch - 'a' + 'A');
        }
        ime_session_push_letter(ch);
        ime_session_take_one_shot_shift();
        break;
    }

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
        if (ime_session_backspace() == IME_CHANGE_NONE && ctx->ta != NULL) {
            lv_textarea_delete_char(ctx->ta);
        }
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
        ctx->lang = (ctx->lang == IME_LANG_CN) ? IME_LANG_EN : IME_LANG_CN;
        ime_session_set_lang(ctx->lang);
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
        break;

    case IME_KEY_NONE:
    default:
        return;
    }

    ime_ui_commit_pending(ctx);
    ime_ui_refresh_all(ctx);
}
