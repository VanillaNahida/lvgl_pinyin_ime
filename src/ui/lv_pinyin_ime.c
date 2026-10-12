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

/* 点"键盘/输入框以外"的地方就收起键盘（实现在文件后半段，销毁回调里要用，先声明） */
static void ime_dismiss_cb(lv_event_t *e);

void ime_root_delete_cb(lv_event_t *e)
{
    lv_pinyin_ime_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL) {
        return;
    }
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    /*
     * ★ 语音必须先摘掉：会话的任务还在跑的时候释放 ctx，它的状态回调就会拿着
     *   已经失效的控件指针去 lv_async_call（悬垂指针，真机上会崩）。
     *   ime_ui_voice_detach() 会停任务、取消挂起的 async，之后才轮到下面摘 dismiss。
     */
    ime_ui_voice_detach(ctx);
#endif
    /*
     * ★ 必须在这里把"点空白收起"的回调摘掉：它挂在**屏幕 / 顶层 layer** 上，
     *   那两个比键盘活得久 —— 不摘的话键盘销毁后它们还留着，一旦再被点到就会用
     *   已释放的 ctx 去 lv_obj_get_user_data()（悬垂指针，真机会崩）。
     *   本回调是 create() 里注册的，ctx 还活着，正好用它的两个字段定位。
     */
    if (ctx->dismiss_root != NULL && lv_obj_is_valid(ctx->dismiss_root)) {
        lv_obj_remove_event_cb_with_user_data(ctx->dismiss_root, ime_dismiss_cb, ctx->obj);
        ctx->dismiss_root = NULL;
    }
    if (ctx->dismiss_layer != NULL && lv_obj_is_valid(ctx->dismiss_layer)) {
        lv_obj_remove_event_cb_with_user_data(ctx->dismiss_layer, ime_dismiss_cb, ctx->obj);
        ctx->dismiss_layer = NULL;
    }
    lv_free(ctx);
}

/*
 * Clamp range for lv_pinyin_ime_set_row_height(). Below 16 px the glyphs no
 * longer fit, above 256 px one row would fill a small screen on its own. The
 * numbers come from the layout theme (ime_layout.h) so "the smallest row the
 * widget will draw" has exactly one definition.
 */
#define IME_UI_ROW_HEIGHT_MIN IME_LAYOUT_MIN_KEY_HEIGHT
#define IME_UI_ROW_HEIGHT_MAX IME_LAYOUT_MAX_KEY_HEIGHT

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
 * *visible* rows, then hand it to the layout engine (which moves the keys).
 *
 * The root is a flex column: pad_all 2 + 2 (top/bottom), pad_row 3 between its
 * children (candidate bar, 9-key pinyin row when visible, key rows container).
 * Hidden children are skipped by the flex layout, so "visible rows" is exactly
 * what the layout measures:
 *
 *   rows_total = 1 (cand bar) + (K9 && 选拼音 ? 1 : 0) + key_rows
 *   fixed      = 4 + 3*(root_children - 1) + 3*(key_rows - 1)
 *   row_height = (fit - fixed) / rows_total
 *
 * ★ 为什么行高在这里算、而不是交给布局引擎从"键盘区高度"反推：键盘区高度本身取决于
 *   候选栏和九键拼音行的高度，而那两行**用的就是同一个行高** —— 交给引擎就成了
 *   "高度取决于行高、行高又取决于高度"。这里一次解出来（行数是已知的），引擎那边
 *   再用 height<=0 → row_height_pref 的路径接手，两边都不需要迭代。
 *   语音页没有这个问题：它只有 kb_rows 一个可见子对象，键盘区高度能直接算出来，
 *   所以那一页走的是引擎的 "height>0 → 反推行高" 那条路。
 *
 * Called before every keyboard build and after every rebuild trigger, so mode /
 * panel / language / T9 switches keep the keyboard filling the same target
 * height.
 */
void ime_ui_fit_row_height(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb)
{
    if (ctx == NULL || kb == NULL || IME_UI_FIT_HEIGHT_PX <= 0) {
        return;
    }
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    /*
     * ★ 语音页不参与这个预算：它没有"键行"，只有三个区段，高度由
     *   voice_page_build() 按"状态行 + 圆形麦克风 + 操作行"自己分摊（候选栏在那一页
     *   是隐藏的，而这里的公式会把它算成一行 —— 早先真机上语音页的圆形麦克风被顶到
     *   288 px 就是这么来的）。行高保持主键盘最后算出来的值，圆和按钮跟着它走。
     */
    if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
        return;
    }
#endif

    const int fit = IME_UI_FIT_HEIGHT_PX;
    const uint32_t key_rows = (uint32_t)kb->row_count;

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
    const uint32_t fixed = 2u * IME_STYLE_ROOT_PAD          /* root pad_all top+bottom */
                         + (uint32_t)IME_STYLE_ROOT_GAP * (root_children - 1u)
                         + (uint32_t)IME_LAYOUT_ROW_GAP * (key_rows > 0 ? key_rows - 1u : 0u);

    int rh = (fit - (int)fixed) / (int)rows_total;
    if (rh < IME_UI_ROW_HEIGHT_MIN) {
        rh = IME_UI_ROW_HEIGHT_MIN;
    } else if (rh > IME_UI_ROW_HEIGHT_MAX) {
        rh = IME_UI_ROW_HEIGHT_MAX;
    }
    IME_UI_ROW_HEIGHT_PX = rh;   /* future rows (rebuilds) start from the derived height */
}

int32_t ime_ui_keyboard_area_height(const lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || IME_UI_FIT_HEIGHT_PX <= 0) {
        return 0;   /* 没有目标高度：布局引擎用 row_height_pref */
    }
    /*
     * The keyboard area is what is left of the fit target once the widget's own
     * padding is taken out. Only the voice page asks for this: there the rows
     * container is the only visible child, so the number is exact. The main
     * keyboard derives its row height in ime_ui_fit_row_height() instead (see the
     * note there) and passes 0.
     */
    if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
        return IME_UI_FIT_HEIGHT_PX - 2 * IME_STYLE_ROOT_PAD;
    }
    return 0;
}

void ime_ui_apply_sizes(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->kb_rows == NULL) {
        return;
    }

    ime_ui_fit_row_height(ctx, ime_ui_active_keyboard(ctx));

    /* The candidate bar and the 9-key pinyin row share the key row height. */
    if (ctx->cand_bar != NULL) {
        lv_obj_set_height(ctx->cand_bar, IME_UI_ROW_HEIGHT_PX);
    }
    if (ctx->t9_row != NULL) {
        lv_obj_set_height(ctx->t9_row, IME_UI_ROW_HEIGHT_PX);
    }

    /* 把新的行高落到已经建好的键上（语音页会整页重建，见 ime_ui_relayout_keyboard）。 */
    ime_ui_relayout_keyboard(ctx);
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
    /*
     * ★ 这里**没有** flex 布局：按键是 kb_rows 的直接子对象，位置和尺寸全部来自
     *   ime_layout 算出来的矩形（行间距也由它给，不再靠容器的 pad_row）。
     *   LV_SIZE_CONTENT 的高度仍由子对象的范围决定（calc_content_height 会算上手动
     *   定位的子对象），所以"容器高度 = 各行之和"这条不变。
     */
    lv_obj_remove_flag(ctx->kb_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ctx->kb_rows, LV_OBJ_FLAG_CLICKABLE);
    /* 宽度/行高变化（第一次排版、旋转、缩放）时重新求解几何。 */
    ime_ui_keyboard_watch_size(ctx);

    /*
     * ★ 先强制跑一次布局，再建键盘：按键用的是**绝对坐标**，而刚建出来的 kb_rows
     *   宽度还是 0（父容器的 LV_PCT(100) 要等一次排版才算得出来）。不先排版的话
     *   第一帧的键会全部挤在 0 宽度上、直到某次 SIZE_CHANGED 才恢复。
     *   父对象此时已经由应用摆好（应用总是先建容器再建键盘），所以这一次排版是确定的。
     */
    lv_obj_update_layout(ctx->obj);

    ime_ui_rebuild_keyboard(ctx);
    ime_ui_apply_sizes(ctx);     /* fit 模式：把行高按目标总高分摊到每一行 */
    ime_ui_refresh_all(ctx);

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    /*
     * 语音：建会话并挂到控件上。没有提供 host 回调时 create() 仍然成功（只是
     * 语音键点了会提示"未配置"），这样"先接键盘、后接网络"的接入顺序是可行的。
     * 会话的 host 由应用通过 lv_pinyin_ime_voice_set_config() 补上。
     */
    (void)ime_ui_voice_attach(ctx->obj);
#endif
    return ctx->obj;
}

/*
 * "点键盘/输入框以外的地方 → 收起键盘"（2026-10 用户报：Wi-Fi 密码与歌单输入的
 * 虚拟键盘在输入框失去激活时不会自己收起来）。
 *
 * 为什么不用 textarea 的 LV_EVENT_DEFOCUSED：LVGL 只在点到**另一个
 * CLICK_FOCUSABLE 对象**时才给旧对象发 DEFOCUSED（见 lv_indev.c 的
 * indev_click_focus），点普通行/遮罩根本不会触发 —— 那正是用户遇到的场景。
 * 所以在屏幕（或顶层 layer，模态框挂那儿）这一层看"这一击落在谁身上"：
 * 落在键盘上、或落在绑定的输入框上，都算"还在输入"。
 *
 * ⚠ 冲突保护：应用自己的点击回调可能顺手 show()（例如点搜索历史行会重跑搜索并
 *   重新拉起键盘），而冒泡到这里的 dismiss 会立刻又把它收起来 ——
 *   所以刚 show() 过的 100 ms 内不理会 dismiss（同本工程 MENU_GRACE_MS 的思路）。
 */
#define IME_DISMISS_GRACE_MS 100

static bool ime_is_under(const lv_obj_t *obj, const lv_obj_t *maybe_ancestor)
{
    for (const lv_obj_t *p = obj; p != NULL; p = lv_obj_get_parent(p)) {
        if (p == maybe_ancestor) {
            return true;
        }
    }
    return false;
}

static void ime_dismiss_cb(lv_event_t *e)
{
    lv_obj_t *ime = (lv_obj_t *)lv_event_get_user_data(e);
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx == NULL || lv_obj_has_flag(ime, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_tick_elaps(ctx->shown_tick) < IME_DISMISS_GRACE_MS) {
        return;
    }
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
    if (tgt == NULL) {
        return;
    }
    if (ime_is_under(tgt, ime) || (ctx->ta != NULL && ime_is_under(tgt, ctx->ta))) {
        return;                          /* 点在键盘上 / 点在输入框上：不算"离开" */
    }
    lv_pinyin_ime_hide(ime);
}

/* 把 dismiss 回调挂到 IME 所在的屏幕与顶层 layer（模态框可能挂在后者上） */
static void ime_bind_dismiss(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx == NULL || ctx->obj == NULL) {
        return;
    }
    lv_obj_t *scr = lv_obj_get_screen(ctx->obj);
    if (scr != NULL && ctx->dismiss_root != scr) {
        lv_obj_add_event_cb(scr, ime_dismiss_cb, LV_EVENT_CLICKED, ctx->obj);
        ctx->dismiss_root = scr;
    }
    lv_obj_t *top = lv_layer_top();
    if (top != NULL && top != scr && ctx->dismiss_layer != top) {
        lv_obj_add_event_cb(top, ime_dismiss_cb, LV_EVENT_CLICKED, ctx->obj);
        ctx->dismiss_layer = top;
    }
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

    /*
     * 光标画成**竖线**（2026-10 用户报："选择输入框时光标是一个方框"）。
     *
     * LVGL 的 textarea 光标是 LV_PART_CURSOR：默认用背景色**填满**整个字符格 →
     * 看起来就是一个实心方块。把它改成"背景透明 + 只留左边框 2px"，就是常见的
     * 细竖线光标。颜色跟随主题的强调色（跟关键字色一致，深浅主题都不会看不见）。
     */
    if (ta != NULL) {
        const lv_color_t cur = lv_obj_get_style_text_color(ta, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_color(ta, cur, LV_PART_CURSOR);
        lv_obj_set_style_border_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
        lv_obj_set_style_radius(ta, 0, LV_PART_CURSOR);
    }

    ime_bind_dismiss(ctx);
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
    lv_pinyin_ime_ctx_t *ctx = ime_ctx_get(ime);
    if (ctx != NULL) {
        ctx->shown_tick = lv_tick_get();
    }
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

    /*
     * 布局本身：这些数字就是"键盘为什么长这样"的答案。行高/标准键宽/网格宽度是求解
     * 结果，后面那一行是自检（越界、重叠、间距、居中、尺寸超限）—— 全 0 才算正常。
     * 真机上键盘看着不对时，先看这几行，而不是去数像素。
     */
    const ime_layout_metrics_t *m = &ctx->kb_metrics;
    IME_LOGI(TAG, "layout: area %dx%d rows=%d row_h=%d unit=%d.%03dpx grid_x=%d grid_w=%d "
                  "gap=%d/%d used_h=%d compact=%d tight=%d",
             (int)m->width, (int)m->height, (int)m->row_count, (int)m->row_height,
             (int)(m->unit_milli / 1000), (int)(m->unit_milli % 1000), (int)m->x_offset,
             (int)m->content_width, (int)m->key_gap, (int)m->row_gap, (int)m->used_height,
             (int)m->compact, (int)m->tight);
    const ime_layout_report_t *rep = &ctx->kb_report;
    IME_LOGI(TAG, "layout check: keys=%u oob=%u overlap=%u gap=%u align=%u w=%u h=%u -> %s",
             (unsigned)rep->keys, (unsigned)rep->out_of_bounds, (unsigned)rep->overlaps,
             (unsigned)rep->bad_gaps, (unsigned)rep->misaligned,
             (unsigned)rep->width_violations, (unsigned)rep->height_violations,
             rep->ok ? "ok" : "PROBLEM");

    /*
     * 按键：全部平铺在 kb_rows 下（没有行容器），所以这里按"每个键自己的矩形"打印。
     * 第一行顺带给出每个键的键面文字 —— 键宽和键面文字是两件最容易对不上的事。
     */
    const uint32_t keys = lv_obj_get_child_count(ctx->kb_rows);
    IME_LOGI(TAG, "%u objects in the keyboard area (voice page: zones, not keys)",
             (unsigned)keys);
    for (uint32_t k = 0; k < keys; k++) {
        lv_obj_t *btn = lv_obj_get_child(ctx->kb_rows, k);
        lv_obj_get_coords(btn, &area);
        const ime_key_t *key = lv_obj_get_user_data(btn);
        IME_LOGI(TAG, "  [%u] %s at (%d,%d) %dx%d", (unsigned)k,
                 (key != NULL && key->label != NULL) ? key->label : "-", (int)area.x1,
                 (int)area.y1, (int)lv_area_get_width(&area), (int)lv_area_get_height(&area));
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
#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
        /*
         * ★ 2026-10（用户要求）：⌨ 键在**三页**之间循环 —— 26 键 → 9 键 → 语音页 → 26 键。
         *
         * 语音不再占用主键盘上的一个键（原来是 LV_SYMBOL_AUDIO 那个，已从键表里删掉），
         * 而是和"切 26/9 键"完全同一套机制：切成一整页，整页就一个大麦克风键 + 返回
         * （键表见 kb.c 的 s_voice_r1 / s_voice_kb，面板是 LV_PINYIN_IME_PANEL_VOICE）。
         *
         * 判断条件只用编译开关：开了语音，键盘建出来时就一定有会话（create() 里那句
         * ime_ui_voice_attach()），所以不需要再问"接没接上"。
         */
        if (ctx->panel == LV_PINYIN_IME_PANEL_VOICE) {
            /* 在语音页按 ⌨ = 回到 26 键 */
            ctx->panel = LV_PINYIN_IME_PANEL_MAIN;
            ctx->panel_alt = false;
            ctx->mode = IME_MODE_K26;
            ime_session_set_mode(ctx->mode);
            ime_ui_rebuild_keyboard(ctx);
            break;
        }
        if (ctx->mode == IME_MODE_K9) {
            /* 26 → 9 → 语音页：第三站是语音 */
            ctx->panel = LV_PINYIN_IME_PANEL_VOICE;
            ctx->panel_alt = false;
            ime_ui_rebuild_keyboard(ctx);
            break;
        }
#endif
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

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
    case IME_KEY_VOICE:
        /*
         * 空操作：语音键的开关走 PRESSED / RELEASED / LONG_PRESSED 三个回调
         * （见 lv_pinyin_ime_kb.c 的 voice_pressed_cb）。这里保持显式分支，
         * 免得将来有人以为漏了处理而在这里补一句 —— 那会和按键回调重复触发。
         */
        break;
#endif

    case IME_KEY_NONE:
    default:
        return;
    }

    ime_ui_commit_pending(ctx);
    ime_ui_refresh_all(ctx);
}
