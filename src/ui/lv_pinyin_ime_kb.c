/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 键盘渲染层：把键表变成一排 LVGL 按钮，并挂上事件回调。
 *
 * ★ 这里**不做任何布局计算**。几何全部来自 ime_layout.c，本文件只负责：
 *
 *      ime_ui_calculate_layout()   ← 量尺寸、求解矩形（纯算术，见 ime_layout.c）
 *      ime_ui_create_key()         ← 一个矩形 = 一个按钮（样式、键面、事件）
 *      ime_ui_apply_keyboard()     ← 清空 + 求解 + 逐个建键
 *
 *   旧实现把这三件事揉在一个函数里，还用 flex_grow 让每行自己分配宽度 —— 于是"键的
 *   宽度"同时取决于父容器内边距、子对象间距、本行权重之和和按钮自身样式，谁都不好
 *   单独调。现在键的矩形是算出来的绝对值，容器不再参与排版。
 *
 * ★ 按键是 kb_rows 的**直接子对象**（没有"行容器"这一层）：层级更浅、矩形直接落到
 *   lv_obj_set_pos()/lv_obj_set_size()，行间距也由布局引擎统一给出，不再靠容器的
 *   pad_row 和 flex 各行其是。
 *
 * 键表本身在 ime_keymaps.c（纯数据，可脱离 LVGL 测试），事件与语音 API 与重构前
 * 完全一致。
 */

#include "lv_pinyin_ime_internal.h"

#include "ime_keymaps.h"
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/* 语音键的位图麦克风（assets/mic.png 烘出来的，见 gen_mic_icon.py）。
 * 只在语音打开时引用：mic_icon 是十几 KB 的常量数组，语音关掉时没有任何键表会用到
 * 它，引用一下就会白白进固件。 */
#include "mic_icon.h"
#endif

#include <ctype.h>
#include <string.h>

#include "core/ime_log.h"
#include "data/ime_font.h"

static const char *TAG = "ime_kb";

/* --------------------------------------------------------------- 键表查找 */

static const ime_keyboard_t *const s_main_kb[IME_MODE_LAST][IME_LANG_LAST] = {
    {&ime_kb_k26_cn, &ime_kb_k26_en},
    {&ime_kb_k9, &ime_kb_k9},
};

static const ime_keyboard_t *const s_panel_kb[LV_PINYIN_IME_PANEL_LAST] = {
    NULL, /* P0 is selected by mode / language */
    &ime_kb_p1,
    &ime_kb_p2,
    &ime_kb_p3,
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    &ime_kb_voice, /* P4: 语音页（这一页自己渲染，见 voice_page_build） */
#endif
};

const ime_keyboard_t *ime_ui_active_keyboard(const lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL) {
        return &ime_kb_k26_cn;
    }
    if (ctx->panel == LV_PINYIN_IME_PANEL_MAIN) {
        ime_mode_t mode = (ctx->mode < IME_MODE_LAST) ? ctx->mode : IME_MODE_K26;
        ime_lang_t lang = (ctx->lang < IME_LANG_LAST) ? ctx->lang : IME_LANG_CN;
        return s_main_kb[mode][lang];
    }
    if (ctx->panel == LV_PINYIN_IME_PANEL_NUM_SYM && ctx->panel_alt) {
        return &ime_kb_p1_alt;
    }
    if (ctx->panel < LV_PINYIN_IME_PANEL_LAST) {
        return s_panel_kb[ctx->panel] ? s_panel_kb[ctx->panel] : &ime_kb_k26_cn;
    }
    return &ime_kb_k26_cn;
}

/* ------------------------------------------------------------------ 几何 */

/** 布局参数：默认主题 + 当前行高（fit 模式会改它，见 ime_ui_fit_row_height）。 */
static void layout_theme(ime_layout_theme_t *theme)
{
    ime_layout_theme_default(theme);
    theme->row_height_pref = IME_UI_ROW_HEIGHT_PX;
}

bool ime_ui_calculate_layout(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb,
                             ime_key_rect_t *rects, size_t cap)
{
    if (ctx == NULL || kb == NULL || rects == NULL || ctx->kb_rows == NULL) {
        return false;
    }

    /*
     * 表本身的校验（空行、width_u 为 0、ref_row 越界、用了没实现的对齐模式）——
     * 这些以前只能靠肉眼看注释发现。放在求解之前，坏表就不会画出一个歪键盘。
     */
    char why[64];
    if (!ime_layout_validate_keyboard(kb, why, sizeof(why))) {
        IME_LOGE(TAG, "keyboard table rejected: %s", why);
        return false;
    }

    const int32_t width = lv_obj_get_content_width(ctx->kb_rows);
    if (width <= 0) {
        /*
         * 控件还没被排版过（宽度是 0）。这不是错误：create() 里会先强制跑一次布局，
         * 而 kb_rows 的 SIZE_CHANGED 回调会在宽度真正算出来之后再补一次。
         */
        return false;
    }

    ime_layout_theme_t theme;
    layout_theme(&theme);

    const int32_t height = ime_ui_keyboard_area_height(ctx);
    if (!ime_layout_measure(&theme, kb, width, height, &ctx->kb_metrics)) {
        IME_LOGW(TAG, "no layout in %dx%d: the gaps leave no room for a key", (int)width,
                 (int)height);
        return false;
    }

    if (!ime_layout_solve(kb, &ctx->kb_metrics, rects, cap)) {
        IME_LOGE(TAG, "layout solve failed (more than %u keys, or a row over budget)",
                 (unsigned)cap);
        return false;
    }

    (void)ime_layout_validate(kb, &ctx->kb_metrics, rects, cap, &ctx->kb_report);
    if (!ctx->kb_report.ok) {
        /* 不阻止渲染：把问题说清楚，画面交给使用者判断（真机上只能靠这几行日志）。 */
        IME_LOGW(TAG, "layout check: oob=%u overlap=%u gap=%u align=%u w=%u h=%u",
                 (unsigned)ctx->kb_report.out_of_bounds, (unsigned)ctx->kb_report.overlaps,
                 (unsigned)ctx->kb_report.bad_gaps, (unsigned)ctx->kb_report.misaligned,
                 (unsigned)ctx->kb_report.width_violations,
                 (unsigned)ctx->kb_report.height_violations);
    }
    IME_UI_UNIT_PX = ime_layout_unit_px(&ctx->kb_metrics);
    return true;
}

/* ------------------------------------------------------------------ 事件 */

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

/* ------------------------------------------------------------- 单个按键 */

/** 键面：语音键画位图麦克风，其余键画文字标签。 */
static void key_build_face(lv_pinyin_ime_ctx_t *ctx, lv_obj_t *btn, const ime_key_t *key)
{
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    if (key->action == IME_KEY_VOICE) {
        /*
         * 位图麦克风，不用 LV_SYMBOL_AUDIO：那是 FontAwesome 的**喇叭**，
         * 表达的是"播放/音量"而不是"说话"；而且它靠字体里刚好有那个私用区字形，
         * 换了 UI 字体就会变成方框。位图与字体无关。
         *
         * 注：主键盘上已经没有语音键了（语音是独立的一页），这一支只为"键表里
         * 真的写了 IME_KEY_VOICE"的情况保留 —— 语音页的圆形麦克风由
         * voice_page_build() 自己建，不走这里，也不再需要 FLOATING + align 那套
         * 让按键脱离布局的 hack。
         */
        lv_obj_t *icon = lv_image_create(btn);
        lv_image_set_src(icon, &mic_icon);
        lv_image_set_scale(icon, 512);   /* 24px 位图放大 2 倍 */
        lv_obj_center(icon);
        return;
    }
#endif

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, key_label(ctx, key));
    lv_label_set_recolor(label, true);
    ime_style_apply_font(label, key_caption_font(key));
    lv_obj_center(label);
}

/** 事件：一个键按下之后会发生什么（与重构前逐条一致）。 */
static void key_bind_events(lv_pinyin_ime_ctx_t *ctx, lv_obj_t *btn, const ime_key_t *key)
{
    if (key->action == IME_KEY_NONE) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        return;
    }

    lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(btn, key_long_press_cb, LV_EVENT_LONG_PRESSED_REPEAT, ctx);

    if (key->action == IME_KEY_SHIFT) {
        /*
         * Latch the shift key while it actually changes what a letter produces:
         * English only (in Chinese the letters are upper case anyway and shift
         * does nothing), and for the one-shot as well as the lock, because the
         * next letter is upper case in both.
         */
        const bool active =
            (ctx->lang == IME_LANG_EN) && (ime_session_shift() != IME_SHIFT_OFF);
        lv_obj_set_state(btn, LV_STATE_CHECKED, active);
    }

    if (key->action == IME_KEY_BACKSPACE) {
        /*
         * ★ PRESS_LOCK: the slide-to-clear gesture moves the finger off this key,
         *   and without the lock LVGL re-hit-tests the finger position on every
         *   press cycle (lv_indev.c pointer_search_obj), hands the events to
         *   whatever is under it and sends PRESS_LOST here - the gesture would
         *   die half way. With the lock this key stays the active object until
         *   the finger lifts.
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
         * 这里不去管它：handle_key 对 IME_KEY_VOICE 是空操作（见 lv_pinyin_ime.c），
         * 不会重复触发。
         */
        lv_obj_add_event_cb(btn, voice_pressed_cb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(btn, voice_released_cb, LV_EVENT_RELEASED, ctx);
        lv_obj_add_event_cb(btn, voice_press_lost_cb, LV_EVENT_PRESS_LOST, ctx);
    }
#endif
}

/**
 * One key = one rectangle.
 *
 * The rect comes from ime_layout_solve(): this function never derives a size from
 * anything but `rect`, so a change in the layout rules cannot silently move the
 * keys somewhere else (and vice versa).
 */
static void ime_ui_create_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key,
                              const ime_key_rect_t *rect)
{
    lv_obj_t *btn = lv_button_create(ctx->kb_rows);

    /*
     * Style first: ime_style_apply_key() starts with lv_obj_remove_style_all(),
     * which also clears local properties already set on the object - including
     * the size below, which in LVGL 9 is stored as a local style.
     */
    ime_style_apply_key(btn, key_is_special(key));
    lv_obj_set_pos(btn, rect->x, rect->y);
    lv_obj_set_size(btn, rect->width, rect->height);
    lv_obj_set_user_data(btn, (void *)key);

    key_build_face(ctx, btn, key);
    key_bind_events(ctx, btn, key);
}

/** 把已经建好的键搬到新矩形上（不新建、不删除对象）。 */
static void keys_move_to(lv_pinyin_ime_ctx_t *ctx, const ime_key_rect_t *rects, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, i);
        if (btn == NULL) {
            return;
        }
        lv_obj_set_pos(btn, rects[i].x, rects[i].y);
        lv_obj_set_size(btn, rects[i].width, rects[i].height);
    }
}

/* --------------------------------------------------------------- 语音页 */

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/* 底部两个按钮的回调 —— 与主键盘上的键走同一套语音组件 API。 */
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
    if (ctx == NULL) {
        return;
    }
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
    if (ctx == NULL) {
        return;
    }
    lv_pinyin_ime_voice_key_cancelled(ctx->obj);   /* 会话没在跑时是空操作 */
    ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
    ctx->panel_alt = false;
    ime_ui_rebuild_keyboard(ctx);
}

/** 三区段里的一个普通容器（无样式、不滚动、不吞点击）。 */
static lv_obj_t *voice_zone_create(lv_pinyin_ime_ctx_t *ctx, const ime_key_rect_t *rect)
{
    lv_obj_t *zone = lv_obj_create(ctx->kb_rows);
    lv_obj_remove_style_all(zone);
    lv_obj_set_pos(zone, rect->x, rect->y);
    lv_obj_set_size(zone, rect->width, rect->height);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    return zone;
}

/*
 * 语音页的版式（用户要求）：
 *
 *     ┌────────────────────────────┐
 *     │      聆听中 / 识别中 / 原因    │  ← 状态区（一行高，文字居中）
 *     │            ◯ 🎤             │  ← 麦克风区：剩下的高度，圆形居中
 *     │      ⌨            返回        │  ← 底部操作区（一行高）
 *     └────────────────────────────┘
 *
 * ★ 尺寸规范与主键盘**同一套**：同一个 ime_layout_theme_t、同一份 12u 网格
 *   （ime_layout_measure_ref(ref_units = 120, ref_keys = 2)），行高就是主键盘的
 *   IME_UI_ROW_HEIGHT_PX，间距就是主题的 key_gap_y。所以屏幕、字号、fit 高度一变，
 *   圆麦克风和底部按钮跟着一起缩放，不会出现"主键盘变大了、语音页还是那 72 px"。
 *
 * ★ 三个区段**都是确定高度**，麦克风靠算出来的余量居中：
 *       H_m = H_k - H_status - H_bottom - 2 * gap
 *       D   = min(D_preferred, 网格宽度, H_m)
 *   不再用 flex_grow —— 上一版依赖"父容器高度能不能触发 grow"，而父容器是
 *   LV_SIZE_CONTENT，实测会算成 0，圆形按钮整颗消失（2026-10 真机）。也不用
 *   LV_OBJ_FLAG_FLOATING 把按钮移出布局系统。
 */
static void voice_page_build(lv_pinyin_ime_ctx_t *ctx)
{
    const int32_t grid_w_avail = lv_obj_get_content_width(ctx->kb_rows);
    if (grid_w_avail <= 0) {
        /* 宽度还没算出来：等 kb_rows 的 SIZE_CHANGED 再建（见 kb_rows_size_cb）。 */
        return;
    }

    ime_layout_theme_t theme;
    layout_theme(&theme);

    /* 与主键盘同一个 12u 网格：底部两个 6u 按钮，按这个参考行反算单位宽度。 */
    ime_layout_metrics_t m;
    if (!ime_layout_measure_ref(&theme, grid_w_avail, 0, 120, 2, &m)) {
        IME_LOGW(TAG, "voice page: no layout in %d px", (int)grid_w_avail);
        return;
    }

    const int32_t row_h = IME_UI_ROW_HEIGHT_PX;
    const int32_t gap = m.row_gap;
    const int32_t grid_x = m.x_offset;
    const int32_t grid_w = m.content_width;
    const int32_t btn_w = 4 * ime_layout_unit_px(&m);   /* 底部两个按钮各 4u */

    /* 圆形直径：跟随键盘行高（3 行高），但不越过可用宽度。 */
    int32_t d = theme.voice_diameter;
    if (d < 3 * row_h) {
        d = 3 * row_h;
    }
    if (d > grid_w) {
        d = grid_w;
    }

    /*
     * 键盘区高度：fit 模式下语音页只剩 kb_rows 一个可见子对象，所以可用高度就是
     * 目标高度减去 root 自己的上下内边距；其余情况按内容排（状态 + 圆 + 操作栏）。
     */
    int32_t area_h = 0;
    if (IME_UI_FIT_HEIGHT_PX > 0) {
        const int32_t fit_h = IME_UI_FIT_HEIGHT_PX - 2 * IME_UI_PAD_Y;
        const int32_t mic_space = fit_h - 2 * row_h - 2 * gap;
        if (mic_space >= row_h) {
            area_h = fit_h;
            if (d > mic_space) {
                d = mic_space;
            }
        }
        /* 目标高度连三行都放不下：忽略它，按内容排，绝不把圆压成负数。 */
    }
    if (area_h <= 0) {
        area_h = 2 * row_h + 2 * gap + d;
    }

    lv_obj_set_height(ctx->kb_rows, area_h);
    ctx->kb_key_count = 0;   /* 这一页没有"键"，只有三个区段 */

    /* 1. 状态区 */
    const ime_key_rect_t status_rect = {grid_x, 0, grid_w, row_h};
    lv_obj_t *status_zone = voice_zone_create(ctx, &status_rect);

    lv_obj_t *st = lv_label_create(status_zone);
    ime_style_apply_font(st, ime_font_small());
    lv_label_set_text(st, "语音输入");
    lv_obj_center(st);
    ctx->voice_page_status = st;

    /* 2. 麦克风区：余量居中 */
    const int32_t mic_space = area_h - 2 * row_h - 2 * gap;
    const ime_key_rect_t mic_rect = {grid_x, row_h + gap, grid_w,
                                     (mic_space > 0) ? mic_space : row_h};
    lv_obj_t *mic_zone = voice_zone_create(ctx, &mic_rect);

    lv_obj_t *mic = lv_button_create(mic_zone);
    ime_style_apply_key(mic, false);
    lv_obj_set_size(mic, d, d);
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

    /* 3. 底部操作区：左 ⌨、右 返回，尺寸同样由布局算出来 */
    const ime_key_rect_t act_rect = {grid_x, area_h - row_h, grid_w, row_h};
    lv_obj_t *act_zone = voice_zone_create(ctx, &act_rect);

    lv_obj_t *kb_btn = lv_button_create(act_zone);
    ime_style_apply_key(kb_btn, true);
    lv_obj_set_pos(kb_btn, 0, 0);
    lv_obj_set_size(kb_btn, btn_w, row_h);
    lv_obj_t *kb_lab = lv_label_create(kb_btn);
    ime_style_apply_font(kb_lab, LV_FONT_DEFAULT);
    lv_label_set_text(kb_lab, LV_SYMBOL_KEYBOARD);
    lv_obj_center(kb_lab);
    lv_obj_add_event_cb(kb_btn, voice_page_back_kb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *back_btn = lv_button_create(act_zone);
    ime_style_apply_key(back_btn, false);
    lv_obj_set_pos(back_btn, grid_w - btn_w, 0);
    lv_obj_set_size(back_btn, btn_w, row_h);
    lv_obj_t *back_lab = lv_label_create(back_btn);
    ime_style_apply_font(back_lab, ime_font_small());
    lv_label_set_text(back_lab, "返回");
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, voice_page_back_done, LV_EVENT_CLICKED, ctx);

    IME_UI_UNIT_PX = ime_layout_unit_px(&m);
}
#endif /* CONFIG_LV_PINYIN_IME_VOICE_ENABLE */

/* ------------------------------------------------------------ 建整块键盘 */

/**
 * 求解结果的中转缓冲（IME_UI_KEY_MAX × 16 字节 = 1 KB）。
 *
 * ★ 放在文件作用域而不是栈上：LVGL 任务的栈本来就不宽裕，而这个缓冲是每次重建/改尺寸
 *   都要用的临时空间，不是递归状态。只在 LVGL 线程里用，所以不需要加锁。
 */
static ime_key_rect_t s_layout_rects[IME_UI_KEY_MAX];

void ime_ui_apply_keyboard(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }
    lv_obj_clean(ctx->kb_rows);
    ctx->kb_key_count = 0;

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    /*
     * ★ 语音页那两个指针必须先清掉：它们指向的对象刚刚被 lv_obj_clean() 删掉了，
     *   留着就是悬垂指针 —— ime_ui_voice_sync() 每次（100 ms）都会拿 voice_page_status
     *   去 lv_label_set_text()，离开语音页之后那一写就是在已释放的对象上（真机会崩）。
     */
    ctx->voice_page_status = NULL;
    ctx->voice_page_mic = NULL;

    if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
        /*
         * 语音页：三区段自己搭；候选栏让位（状态由那一行文字承担）。
         *
         * ★ kb_rows 平时是 LV_SIZE_CONTENT（高度 = 各行之和），而语音页内容少，
         *   差出来的那截就成了输入框下面那段**大白**（用户报）。所以语音页把
         *   kb_rows 的高度**算成确定值**（fit 模式用目标高度，否则按内容），
         *   中间的圆形键去吃掉多余空间 —— 而不是靠 flex_grow 撞运气。
         */
        if (ctx->cand_bar != NULL) {
            lv_obj_add_flag(ctx->cand_bar, LV_OBJ_FLAG_HIDDEN);
        }
        voice_page_build(ctx);
        return;
    }
    if (ctx->cand_bar != NULL) {
        lv_obj_remove_flag(ctx->cand_bar, LV_OBJ_FLAG_HIDDEN);
    }
    /*
     * ★ 回到普通键盘时**必须把高度还原成 LV_SIZE_CONTENT**：语音页给它设过确定高度
     *   （fit 模式下是"目标高度 - 边距"，比键行本身高得多），不还原的话键盘下面会留出
     *   一整条空白 —— 用户报的"从语音页返回 26 键后空格下面多一块留白"就是这个。
     */
    lv_obj_set_height(ctx->kb_rows, LV_SIZE_CONTENT);
#endif

    if (kb == NULL) {
        IME_LOGW(TAG, "no keyboard for mode %d panel %d", (int)ctx->mode, (int)ctx->panel);
        return;
    }

    if (!ime_ui_calculate_layout(ctx, kb, s_layout_rects, IME_UI_KEY_MAX)) {
        return;   /* 原因已经打过日志；下一次 SIZE_CHANGED / rebuild 会再来一次 */
    }

    size_t idx = 0;
    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];
        for (size_t k = 0; k < row->count; k++) {
            ime_ui_create_key(ctx, &row->keys[k], &s_layout_rects[idx]);
            idx++;
        }
    }
    ctx->kb_key_count = idx;
}

void ime_ui_relayout_keyboard(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }

    const ime_keyboard_t *kb = ime_ui_active_keyboard(ctx);

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
        /* 语音页的区段高度依赖可用宽高：整页重建是最省事也最不容易错的做法。 */
        ime_ui_apply_keyboard(ctx, kb);
        return;
    }
#endif

    if (kb == NULL) {
        return;
    }

    const size_t want = ime_layout_key_count(kb);
    if (ctx->kb_key_count != want || lv_obj_get_child_count(ctx->kb_rows) != want) {
        /*
         * 还没有键（第一次排版前建的键盘，宽度是 0）或者键数对不上（页面刚换过）：
         * 这一条路径才会新建对象。
         */
        ime_ui_apply_keyboard(ctx, kb);
        return;
    }

    if (!ime_ui_calculate_layout(ctx, kb, s_layout_rects, IME_UI_KEY_MAX)) {
        return;
    }
    keys_move_to(ctx, s_layout_rects, want);
}

/**
 * kb_rows 的尺寸变了（第一次排版算出宽度、旋转、窗口缩放）：重新求解几何。
 *
 * ★ 只在**宽度真的变了**的时候动手。这个回调是在布局过程中发出来的，而键的新尺寸
 *   又会改变 kb_rows 的内容高度、再触发一次自身尺寸刷新 —— 不设这道闸就是"父对象多高
 *   取决于子对象、子对象位置又取决于父对象"的自反馈（AGENTS §12.0 那个看门狗）。
 *   宽度不变时直接返回，所以最坏情况只多跑一轮布局。
 */
static void kb_rows_size_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }
    const int32_t w = lv_obj_get_content_width(ctx->kb_rows);
    if (w == ctx->kb_area_w) {
        return;
    }
    ctx->kb_area_w = w;
    ime_ui_relayout_keyboard(ctx);
}

void ime_ui_rebuild_keyboard(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }
    const ime_keyboard_t *kb = ime_ui_active_keyboard(ctx);
    if (kb == NULL) {
        IME_LOGW(TAG, "no keyboard for mode %d panel %d", (int)ctx->mode, (int)ctx->panel);
        return;
    }

    /*
     * ★ 先算行高、再建键：fit 模式下每行的行高由"目标高度 / 可见行数"决定，而行数
     *   取决于**即将建出来的这张表**（候选栏 + 可选的九键拼音行 + 键盘行）。反过来的
     *   话，新建出来的键会先按上一页的行高排一次、再由 ime_ui_apply_sizes() 挪一次，
     *   白白多一遍布局。
     */
    ime_ui_fit_row_height(ctx, kb);
    ime_ui_apply_keyboard(ctx, kb);
}

/** 把 kb_rows 的尺寸回调挂上（create() 里调一次）。 */
void ime_ui_keyboard_watch_size(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }
    lv_obj_add_event_cb(ctx->kb_rows, kb_rows_size_cb, LV_EVENT_SIZE_CHANGED, ctx);
}
