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
    /*
     * 翻页键也要用输入法自己的字体：不设的话标签会用 LV_FONT_DEFAULT
     * （montserrat），而箭头字形未必在那一套里 —— 那样 pager_caption() 的
     * 字形探测就白探了（探的是小字体、画的却是默认字体，最后画成方框）。
     */
    ime_style_apply_font(label, ime_font_small());
    lv_obj_center(label);
    return btn;
}

/* ------------------------------------------------------------------ 拼音浮窗 */

/** 与 s_chip 的 pad_ver 一致（算浮窗高度时要用）。 */
#define IME_UI_CHIP_PAD_VER 2

/**
 * 浮窗能画到内容区之上多高。
 *
 * LVGL 把子对象裁在父对象的盒子里，除非父对象带 LV_OBJ_FLAG_OVERFLOW_VISIBLE
 * —— 而那时可用区域是"盒子 + ext_draw_size"。所以要让浮窗画到控件外面，光有
 * OVERFLOW_VISIBLE 不够，还得把扩展绘制区撑到浮窗那么高。
 */
#define IME_UI_CHIP_OVERFLOW 28

/** 取字符串首字符的码点（LVGL 9 的解码器在 private 头里，不能直接用）。 */
static uint32_t utf8_first_cp(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        return ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0) {
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
               (uint32_t)(p[2] & 0x3F);
    }
    return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
}

/**
 * 翻页按钮的图标：优先用 LVGL 自带的左右箭头（LV_SYMBOL_LEFT / RIGHT）。
 *
 * 这两个图标在 FontAwesome 私用区（U+F053 / U+F054），组件自带字体的字符集
 * （tools/gen_charset.py）里没有它们 —— 那里收的是 Unicode 箭头 ← →。
 * 所以先问字体有没有这个字形，没有就退回 Unicode 箭头（已经在字符集里），
 * 别的工程直接拿组件跑也不会画成空框。装了更全字体的工程
 * （lv_pinyin_ime_set_fonts()，比如本工程用整套 UI 位图字体）用到的就是图标。
 */
static const char *pager_caption(int dir)
{
    const char *sym = (dir < 0) ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT;
    const lv_font_t *font = ime_font_small();
    if (font == NULL) {
        /* 没装字体时键面用 LV_FONT_DEFAULT（montserrat），它带这些图标 */
        return sym;
    }
    /*
     * ⚠ 不能只看 lv_font_get_glyph_dsc() 的返回值：字体缺字时它**返回 true**，
     *   只是把 is_placeholder 置起来（缺字画成方框）。所以要看那个标志。
     */
    lv_font_glyph_dsc_t dsc;
    if (lv_font_get_glyph_dsc(font, &dsc, utf8_first_cp(sym), '\0') && !dsc.is_placeholder) {
        return sym;
    }
    return (dir < 0) ? "\xE2\x86\x90" : "\xE2\x86\x92";   /* ← / → */
}

static void chip_ext_draw_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, IME_UI_CHIP_OVERFLOW);
}

void ime_ui_chip_prepare(lv_obj_t *root)
{
    /*
     * 让浮窗能画到控件上方：控件自己**和它的每一层祖先**都要 (1) 允许子对象溢出、
     * (2) 把扩展绘制区加高 —— 少一层就会被那一层裁掉，而 App 里包键盘的容器往往
     * 正好是 LV_SIZE_CONTENT 紧贴键盘（那种最容易咬到）。
     *
     * 只加一个标志 + 一个不用 user_data 的回调，纯几何影响，不改容器其它行为。
     */
    for (lv_obj_t *o = root; o != NULL; o = lv_obj_get_parent(o)) {
        lv_obj_remove_event_cb(o, chip_ext_draw_cb);
        lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(o, chip_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(o);
    }
}

/** 把浮窗摆到候选栏上沿之上（内容区的第一行就是候选栏）。 */
static void chip_place(lv_pinyin_ime_ctx_t *ctx)
{
    const lv_font_t *font = ime_font_small();
    const int line_h = (font != NULL) ? (int)font->line_height : 20;
    const int chip_h = line_h + 2 * IME_UI_CHIP_PAD_VER;
    /*
     * 静态对齐，**不用** lv_obj_align_to(候选栏)：那个会记住 base 并在 base 移动时
     * 重新对齐，而候选栏的位置由父对象布局决定 —— 正好是自反馈的配方（见 AGENTS
     * §12.0 那个看门狗）。内容区第一行就是候选栏，所以"候选栏上方"就是内容区
     * y = -(芯片高 + 1)。
     */
    lv_obj_align(ctx->chip, LV_ALIGN_TOP_LEFT, IME_UI_PAD + 2, -(chip_h + 1));
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

    ctx->page_prev = make_pager(ctx->cand_bar, ctx, pager_caption(-1), -1);

    /*
     * 拼音浮窗：画在**候选栏上方**（控件内容区之外，靠 ime_ui_chip_prepare() 的
     * 溢出设置才画得出来），只在有拼音时显示，没拼音时隐藏。
     *
     * ★ FLOATING 是关键：flex 布局与 calc_content_height() **都会**跳过它，所以
     *   浮窗既不占候选栏的宽度、也不影响控件高度 —— 不用预留任何空带（上一版的
     *   常驻空带就是一条灰色的难看条）。
     *
     *   ⚠ 别改成 IGNORE_LAYOUT：那只能让它不参与布局，仍然会算进父对象的内容高度，
     *     "父对象多高取决于子对象、子对象位置又取决于父对象"就成了自反馈，LVGL 会
     *     一直重排（AGENTS §12.0 里那个看门狗）。
     */
    ctx->chip = lv_label_create(ctx->obj);
    lv_label_set_text(ctx->chip, "");
    ime_style_apply_chip(ctx->chip);
    ime_style_apply_font(ctx->chip, ime_font_small());
    lv_obj_remove_flag(ctx->chip, LV_OBJ_FLAG_CLICKABLE);
    /* 长拼音（"zhonghuarenmingongheguo"）不至于横着铺满整屏 */
    lv_label_set_long_mode(ctx->chip, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(ctx->chip, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);
    chip_place(ctx);

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
        ime_style_apply_candidate(btn, false, false);
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

    ctx->page_next = make_pager(ctx->cand_bar, ctx, pager_caption(1), 1);

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
        ime_style_apply_candidate(btn, false, false);
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

/*
 * 一个候选按钮除文字之外占的横向宽度：s_cand_btn 的 pad_hor(4) × 2 加候选行
 * 的 pad_column(2)。量"这一屏能放几个"时要算进去，否则会多放一个、最后一个
 * 被裁掉半个字。
 */
#define IME_UI_CAND_CHROME 10

typedef struct {
    size_t count;                     /* 这一屏放几个 */
    int    pct[IME_UI_CAND_MAX];      /* 每个候选按钮占候选行**内容宽度**的百分比 */
} cand_fit_t;

/**
 * 量出这一屏放几个候选、各自占多宽。
 *
 * ★ 为什么必须量：候选长度差得很远 —— 拼音短的时候是一堆单字（各 20 px），
 *   打长了会出"中华人民共和国"这种 140 px 的词。原来固定按 8 个平分，长词就把后面的
 *   候选顶到可视范围之外（用户报的"有字在显示范围外"）。
 *
 * ★ 为什么还要**显式给宽度**：候选按钮原来是 LV_SIZE_CONTENT + flex_grow=1，而 flex
 *   的基准宽度取的是子对象**当前**宽度 —— 刚换过文字的按钮基准还是旧值，于是同一个
 *   flex 行里 140 px 的长词和 20 px 的单字被摊成一样宽，长词按钮装不下自己的文字，
 *   标签就溢到邻居上。
 *
 * ★ 为什么给的是**百分比**而不是像素：候选行的宽度要等布局跑完才准（条内的翻页键、
 *   控件宽度都可能刚变过），按"量的时候那个宽度"算像素，最后几个候选就会差出十几
 *   像素、探出右边界。百分比在布局时按真实宽度换算，且各项之和 ≤ 100%，永远不会
 *   超界。
 */
static void cand_measure(lv_pinyin_ime_ctx_t *ctx, cand_fit_t *fit)
{
    const int row_w = lv_obj_get_content_width(ctx->cand_row);
    const lv_font_t *font = ime_font_big();
    const size_t start = ime_session_page_start();

    fit->count = 0;
    if (row_w <= 0) {
        /* 还没跑过布局（刚建出来）：先按上限放，下一次刷新就量准了 */
        fit->count = IME_UI_CAND_MAX;
        return;
    }

    int w[IME_UI_CAND_MAX];
    int avail = row_w;
    size_t n = 0;
    for (size_t i = 0; i < IME_UI_CAND_MAX; i++) {
        const char *text = ime_session_candidate(start + i);
        if (text == NULL) {
            break;
        }
        /* lv_text_get_width() 在 LVGL 9 是 private 的，公开入口是 get_size */
        lv_point_t sz = { 0, 0 };
        lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int wi = sz.x + IME_UI_CAND_CHROME;
        if (wi > row_w) {
            wi = row_w;         /* 单个候选就比整行宽：让它自己省略号，别撑破 */
        }
        if (n > 0 && wi > avail) {
            break;
        }
        w[n] = wi;
        avail -= wi;
        n++;
    }
    if (n == 0) {
        fit->count = 1;
        fit->pct[0] = 100;
        return;
    }
    /* 剩下的宽度平分（保持"铺满整行"的观感），再整体换成百分比 */
    const int extra = (avail > 0) ? (avail / (int)n) : 0;
    for (size_t i = 0; i < n; i++) {
        int pct = ((w[i] + extra) * 100) / row_w;
        if (pct < 1) {
            pct = 1;
        }
        fit->pct[i] = pct;
    }
    fit->count = n;
}

void ime_ui_refresh_candidates(lv_pinyin_ime_ctx_t *ctx)
{
    const char *pinyin = ime_session_pinyin();
    bool has_input = (pinyin != NULL && pinyin[0] != '\0');

    /*
     * 浮窗属于 26 键：九键那边拼音在"选拼音"行里，两个都显示就是同一句话印两遍。
     * 只在有拼音缓冲时出现（"没有的时候隐藏，也不需要单独留空间"）。
     */
    if (has_input && ctx->mode == IME_MODE_K26) {
        /* Show the syllables apart: "la'wan'le" rather than "lawanle". */
        ime_session_pinyin_display(ctx->pinyin_text, sizeof(ctx->pinyin_text));
        lv_label_set_text(ctx->chip, ctx->pinyin_text);
        chip_place(ctx);   /* 文本变了高度可能变（换行/字体），位置跟着重算 */
        lv_obj_remove_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->chip, LV_OBJ_FLAG_HIDDEN);
    }

    /* 先量字宽，把"一屏几个"告诉会话（翻页步长与它一致），再读页窗 */
    cand_fit_t fit;
    cand_measure(ctx, &fit);
    ime_session_set_page_size(fit.count);

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
        ime_style_apply_candidate(ctx->cand_btns[i], page_start + i == selected, false);
        /*
         * ★ 宽度按量出来的百分比定死，而且必须**在样式之后**设：本工程用的 LVGL 9
         *   里 lv_obj_set_width() 就是一条本地样式，而 ime_style_apply_candidate()
         *   开头的 lv_obj_remove_style_all() 会把它连同其它本地属性一起清掉
         *   （文件开头 make_pager 那条注释说的也是这件事）。
         */
        if (i < fit.count) {
            lv_obj_set_width(ctx->cand_btns[i], LV_PCT(fit.pct[i]));
        }
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
            ime_style_apply_candidate(ctx->t9_btns[i], i == selected, true);
        } else {
            lv_obj_add_flag(ctx->t9_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
