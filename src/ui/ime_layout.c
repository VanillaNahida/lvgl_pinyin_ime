/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 键盘几何求解器。输入是键盘表 + 键盘区尺寸，输出是每个键的像素矩形。
 *
 * 三条设计原则（对应《输入法布局优化指南》的方案 A + D）：
 *
 * 1. **统一逻辑网格**：所有行共享同一个标准键宽 U。U 由"参考行"反算
 *    （26 键 R1 = 120 个十分之一单位、12 个键），而不是让每行各自分配剩余空间 ——
 *    旧实现用 flex_grow，每行把自己的可用宽度按**本行**权重重新分配，于是"10 单位"
 *    在不同行、不同页面对应不同像素，字母键在 R1 和 R3 里宽度并不相同。
 *
 * 2. **先算几何、再建控件**：这里只做算术，不碰 LVGL。渲染层拿到矩形后调
 *    lv_obj_set_pos()/lv_obj_set_size() 即可，事件逻辑完全不必知道布局是怎么来的。
 *
 * 3. **高度也是算出来的**：行高由键盘区高度、行数与行间距共同决定，空间不够时先压缩
 *    间距与留白，再退到最小触控高度并把"没放下"如实报出来（tight），而不是让按键
 *    互相重叠。
 *
 * 整数与取整：单位宽度用 1/1000 px 定点保存（unit_milli），只在生成矩形时取整，且
 * 按**累计值**取整（第 i 个键的右边界 = round(前缀权重 * U / 10)），所以同一行内不会
 * 累积取整误差、最后一个键总是正好落在行的右边界上。
 */

#include "ime_layout.h"

#include <string.h>

/* --------------------------------------------------------------- defaults */

void ime_layout_theme_default(ime_layout_theme_t *theme)
{
    if (theme == NULL) {
        return;
    }
    memset(theme, 0, sizeof(*theme));

    /*
     * ★ 外边距为 0 是**有意的**：键盘区的坐标原点是控件里的 kb_rows 容器，控件 root
     *   的样式已经带了 pad_all(2) + pad_row(3)。在这里再减一遍就是同一份间距算两次
     *   （旧 update_unit_px() 正好这么干过）。真要在键盘区内部再留白，先把 root 的
     *   样式去掉再改这里。
     */
    theme->outer_pad_x = 0;
    theme->outer_pad_y = 0;
    theme->key_gap_x = IME_LAYOUT_KEY_GAP;
    theme->key_gap_y = IME_LAYOUT_ROW_GAP;

    /* 触控下界：低于 16 px 的键在电阻屏上基本按不准；上界保证大屏上键不会无限变大。 */
    theme->min_key_width = IME_LAYOUT_MIN_KEY_WIDTH;
    theme->max_key_width = IME_LAYOUT_MAX_KEY_WIDTH;
    theme->min_key_height = IME_LAYOUT_MIN_KEY_HEIGHT;
    theme->max_key_height = IME_LAYOUT_MAX_KEY_HEIGHT;

    /* 空间紧张时先动的两个旋钮（比缩键更划算：留白少了但键还按得准）。 */
    theme->compact_key_gap_x = 2;
    theme->compact_key_gap_y = 2;
    theme->compact_pad_x = 1;
    theme->compact_pad_y = 1;

    theme->row_height_pref = IME_LAYOUT_ROW_HEIGHT_DEFAULT;
    theme->voice_diameter = 72;
}

/* ---------------------------------------------------------------- helpers */

/** Sum of the row weights, rejecting the rows the solver cannot honour. */
static int64_t row_weight_sum(const ime_row_t *row)
{
    int64_t sum = 0;

    for (size_t i = 0; i < row->count; i++) {
        if (row->keys[i].width_u > 0) {
            sum += row->keys[i].width_u;
        }
    }
    return sum;
}

static bool row_is_solvable(const ime_row_t *row)
{
    if (row == NULL || row->keys == NULL || row->count == 0) {
        return false;
    }
    if (row->align != IME_ROW_ALIGN_FILL && row->align != IME_ROW_ALIGN_CENTER) {
        return false;   /* GRID: reserved, see ime_layout.h */
    }
    /* Every key needs a positive weight: 0 has no meaning in either mode. */
    for (size_t i = 0; i < row->count; i++) {
        if (row->keys[i].width_u == 0) {
            return false;
        }
    }
    return true;
}

size_t ime_layout_key_count(const ime_keyboard_t *kb)
{
    size_t total = 0;

    if (kb == NULL || kb->rows == NULL) {
        return 0;
    }
    for (size_t r = 0; r < kb->row_count; r++) {
        total += kb->rows[r].count;
    }
    return total;
}

const char *ime_layout_align_name(ime_row_align_t align)
{
    switch (align) {
    case IME_ROW_ALIGN_FILL:
        return "FILL";
    case IME_ROW_ALIGN_CENTER:
        return "CENTER";
    case IME_ROW_ALIGN_GRID:
        return "GRID";
    default:
        return "?";
    }
}

/**
 * Store @p why (when there is room) and fail.
 *
 * Plain strings rather than printf: this runs on the ESP32 where the reason only
 * ever ends up in one log line, and a format string would drag <stdio.h> into a
 * module that is otherwise pure arithmetic.
 */
static bool kb_reject(char *err, size_t err_size, const char *why)
{
    if (err != NULL && err_size > 0) {
        size_t i = 0;
        for (; why[i] != '\0' && i + 1 < err_size; i++) {
            err[i] = why[i];
        }
        err[i] = '\0';
    }
    return false;
}

bool ime_layout_validate_keyboard(const ime_keyboard_t *kb, char *err, size_t err_size)
{
    if (kb == NULL || kb->rows == NULL || kb->row_count == 0) {
        return kb_reject(err, err_size, "keyboard has no rows");
    }
    if (kb->ref_row >= kb->row_count) {
        return kb_reject(err, err_size, "ref_row is out of range");
    }
    if (!row_is_solvable(&kb->rows[kb->ref_row])) {
        return kb_reject(err, err_size, "the reference row is not solvable");
    }

    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];

        if (row->keys == NULL || row->count == 0) {
            return kb_reject(err, err_size, "a row is empty");
        }
        if (row->align != IME_ROW_ALIGN_FILL && row->align != IME_ROW_ALIGN_CENTER) {
            return kb_reject(err, err_size,
                             "a row uses GRID, which the solver does not implement");
        }
        for (size_t k = 0; k < row->count; k++) {
            const ime_key_t *key = &row->keys[k];

            if (key->width_u == 0) {
                return kb_reject(err, err_size, "a key has width_u 0");
            }
            if (key->label == NULL) {
                return kb_reject(err, err_size, "a key has no label");
            }
            /* A letter key's label is what gets typed, so an empty one is a bug. */
            if (key->action == IME_KEY_LETTER && key->label[0] == '\0') {
                return kb_reject(err, err_size, "a letter key has no label");
            }
        }
    }
    return true;
}

/* --------------------------------------------------------------- measuring */

typedef struct {
    int32_t width;
    int32_t height;
    int32_t rows;
    int32_t ref_units;
    int32_t ref_keys;
    const ime_keyboard_t *kb;   /**< NULL for ime_layout_measure_ref() */
} measure_req_t;

typedef enum {
    GRID_FITS = 0,   /**< unit >= min_key_width */
    GRID_NARROW,     /**< solvable, but the keys came out below the touch floor */
    GRID_IMPOSSIBLE, /**< the gaps alone eat the row */
} grid_fit_t;

/**
 * Standard key width for one (margin, gap) combination.
 *
 * U = (content - (ref_keys - 1) * gap) / (ref_units / 10)
 *
 * `ref_units` is in tenths, so dividing by ref_units/10 units is what turns the
 * reference row's 120 tenths into "12 keys wide". Keeping the result in 1/1000 px
 * means a 320 px screen (U = 23.58 px) does not lose 0.6 px per key to rounding.
 */
static grid_fit_t measure_unit(const ime_layout_theme_t *theme, const measure_req_t *req,
                               int32_t pad_x, int32_t gap_x, int32_t *content_out,
                               int32_t *unit_milli_out)
{
    const int32_t content = req->width - 2 * pad_x;
    if (content <= 0) {
        return GRID_IMPOSSIBLE;
    }
    const int64_t numerator = (int64_t)content - (int64_t)(req->ref_keys - 1) * gap_x;
    if (numerator <= 0) {
        return GRID_IMPOSSIBLE;
    }
    const int64_t unit = numerator * 10000 / req->ref_units;
    if (unit <= 0) {
        return GRID_IMPOSSIBLE;
    }

    *content_out = content;
    *unit_milli_out = (int32_t)unit;
    if (theme->min_key_width > 0 && unit < (int64_t)theme->min_key_width * 1000) {
        return GRID_NARROW;
    }
    return GRID_FITS;
}

/** Row height for one (vertical margin, row gap) combination. */
static int32_t measure_row_height(const measure_req_t *req, int32_t pad_y, int32_t gap_y)
{
    const int64_t free_h = (int64_t)req->height - 2 * pad_y - (int64_t)(req->rows - 1) * gap_y;
    if (free_h <= 0) {
        return 0;
    }
    return (int32_t)(free_h / req->rows);
}

/**
 * Can every CENTER row be rounded to the nearest pixel and still fit the grid?
 *
 * U is derived so that the *reference* row fills the grid exactly, so rounding its
 * keys up by up to 0.5 px each can push a row over the edge. When that happens the
 * whole keyboard switches to rounding down (see metrics.round_widths) - one pixel
 * narrower keys everywhere is invisible, whereas mixing the two modes inside one
 * keyboard is not (the same 0.9u key would be 17 px on one row and 18 px on the
 * next, which is exactly the "columns do not line up" complaint).
 */
static bool center_rows_fit_rounded(const ime_keyboard_t *kb, int32_t content_width, int32_t gap,
                                    int32_t unit_milli)
{
    if (kb == NULL) {
        return true;   /* no rows to check: the caller describes its own grid */
    }
    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];
        if (row->align != IME_ROW_ALIGN_CENTER || row->keys == NULL) {
            continue;
        }
        int64_t width = (int64_t)(row->count - 1) * gap;
        for (size_t k = 0; k < row->count; k++) {
            width += ((int64_t)row->keys[k].width_u * unit_milli + 5000) / 10000;
        }
        if (width > content_width) {
            return false;
        }
    }
    return true;
}

static bool measure_core(const ime_layout_theme_t *theme, const measure_req_t *req,
                         ime_layout_metrics_t *out)
{
    if (theme == NULL || req == NULL || out == NULL) {
        return false;
    }
    if (req->width <= 0 || req->rows <= 0 || req->ref_units <= 0 || req->ref_keys <= 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->width = req->width;
    out->height = req->height;
    out->row_count = req->rows;
    out->ref_units = req->ref_units;
    out->ref_keys = req->ref_keys;
    out->min_key_width = theme->min_key_width;
    out->max_key_width = theme->max_key_width;
    out->min_key_height = theme->min_key_height;
    out->max_key_height = theme->max_key_height;

    int32_t pad_x = theme->outer_pad_x;
    int32_t pad_y = theme->outer_pad_y;
    int32_t gap_x = theme->key_gap_x;
    int32_t gap_y = theme->key_gap_y;
    int32_t content = 0;
    int32_t unit_milli = 0;
    bool compact = false;
    bool tight = false;

    /*
     * Width. One compact attempt, and only when the normal gaps cannot reach the
     * touch floor: compacting when it does not help would just make the keyboard
     * uglier for nothing.
     */
    grid_fit_t fit = measure_unit(theme, req, pad_x, gap_x, &content, &unit_milli);
    if (fit != GRID_FITS) {
        int32_t c2 = 0, u2 = 0;
        const grid_fit_t fit2 =
            measure_unit(theme, req, theme->compact_pad_x, theme->compact_key_gap_x, &c2, &u2);

        if (fit2 == GRID_FITS || (fit == GRID_IMPOSSIBLE && fit2 != GRID_IMPOSSIBLE)) {
            pad_x = theme->compact_pad_x;
            gap_x = theme->compact_key_gap_x;
            content = c2;
            unit_milli = u2;
            fit = fit2;
            compact = true;
        }
        if (fit == GRID_IMPOSSIBLE) {
            return false;   /* the gaps alone eat the row: no layout exists */
        }
        if (fit == GRID_NARROW) {
            tight = true;   /* fits, but below the touch floor (reported, not hidden) */
        }
    }

    /*
     * Ceiling on the key width. On a wide screen the grid must stop growing: the
     * content box shrinks to what the capped unit produces and is centered, so the
     * keys keep their proportions and FILL rows cannot blow one key up to half the
     * screen (guide §8: "剩余空间通过边距或内容区策略消化").
     */
    if (theme->max_key_width > 0 && unit_milli > theme->max_key_width * 1000) {
        unit_milli = theme->max_key_width * 1000;
        const int64_t ref_px = ((int64_t)unit_milli * req->ref_units + 5000) / 10000;
        const int64_t capped = ref_px + (int64_t)(req->ref_keys - 1) * gap_x;
        if (capped < content) {
            content = (int32_t)capped;
        }
    }

    /*
     * Height. With a definite area the row height is derived from it; otherwise the
     * caller's preferred row height is used (content-sized widget). Same compact
     * policy as the width: shrink the gaps first, then accept the floor and say so.
     */
    int32_t row_height;
    if (req->height > 0) {
        row_height = measure_row_height(req, pad_y, gap_y);
        if (row_height < theme->min_key_height) {
            const int32_t compact_h = measure_row_height(req, theme->compact_pad_y,
                                                         theme->compact_key_gap_y);
            if (compact_h > row_height) {
                pad_y = theme->compact_pad_y;
                gap_y = theme->compact_key_gap_y;
                row_height = compact_h;
                compact = true;
            }
        }
        if (row_height < theme->min_key_height) {
            row_height = theme->min_key_height;
            tight = true;   /* the caller asked for less than one touch row: report it */
        }
        if (theme->max_key_height > 0 && row_height > theme->max_key_height) {
            row_height = theme->max_key_height;
        }
    } else {
        row_height = theme->row_height_pref;
        if (row_height < theme->min_key_height) {
            row_height = theme->min_key_height;
        }
        if (theme->max_key_height > 0 && row_height > theme->max_key_height) {
            row_height = theme->max_key_height;
        }
    }

    out->pad_left = pad_x;
    out->pad_right = pad_x;
    out->pad_top = pad_y;
    out->pad_bottom = pad_y;
    out->key_gap = gap_x;
    out->row_gap = gap_y;
    out->row_height = row_height;
    out->unit_milli = unit_milli;
    out->content_width = content;
    out->round_widths = center_rows_fit_rounded(req->kb, content, gap_x, unit_milli);
    /* Center the (possibly capped) grid inside the area, margins included. */
    out->x_offset = pad_x + (req->width - 2 * pad_x - content) / 2;
    out->used_height = 2 * pad_y + req->rows * row_height + (req->rows - 1) * gap_y;
    out->compact = compact;
    out->tight = tight;
    return true;
}

bool ime_layout_measure(const ime_layout_theme_t *theme, const ime_keyboard_t *kb,
                        int32_t width, int32_t height, ime_layout_metrics_t *out)
{
    if (kb == NULL || kb->rows == NULL || kb->row_count == 0 ||
        kb->ref_row >= kb->row_count) {
        return false;
    }
    const ime_row_t *ref = &kb->rows[kb->ref_row];
    const int64_t ref_sum = row_weight_sum(ref);

    if (ref_sum <= 0 || ref->count == 0) {
        return false;
    }

    const measure_req_t req = {
        .width = width,
        .height = height,
        .rows = (int32_t)kb->row_count,
        .ref_units = (int32_t)ref_sum,
        .ref_keys = (int32_t)ref->count,
        .kb = kb,
    };
    return measure_core(theme, &req, out);
}

bool ime_layout_measure_ref(const ime_layout_theme_t *theme, int32_t width, int32_t height,
                            int32_t ref_units, int32_t ref_keys, ime_layout_metrics_t *out)
{
    const measure_req_t req = {
        .width = width,
        .height = height,
        .rows = 1,
        .ref_units = ref_units,
        .ref_keys = ref_keys,
        .kb = NULL,   /* the caller describes its own grid: nothing to check */
    };
    return measure_core(theme, &req, out);
}

int32_t ime_layout_unit_px(const ime_layout_metrics_t *metrics)
{
    if (metrics == NULL) {
        return 0;
    }
    return (metrics->unit_milli + 500) / 1000;
}

/* ----------------------------------------------------------------- solving */

/**
 * FILL row: the weights are relative, so the key area (grid width minus the gaps)
 * is handed out in proportion.
 *
 * ★ 用**最大余数法**（先各取整数部分，再把剩下的像素发给小数部分最大的键），
 *   而不是"按累计值取整"。两者的区别在等权重的键上最明显：
 *
 *     累计取整：  20/20/30/25/25 会分到 50/51/76/63/63 —— 同样 20% 的两个角键
 *                 差 1 px（底部功能行两侧的键看着不一样宽）。
 *     最大余数法：50.67/50.67/76/63.33/63.33 -> 50/50/76/63/63，剩 2 px 正好发给
 *                 小数部分最大的那两个 -> 51/51/76/63/63：**等权重的键一定等宽**，
 *                 而且总宽（含间距）严格等于网格宽度，一个像素都不剩。
 */
static bool solve_row_fill(const ime_row_t *row, const ime_layout_metrics_t *m, int32_t y,
                           int32_t base_x, ime_key_rect_t *rects)
{
    const int64_t gap_total = (int64_t)(row->count - 1) * m->key_gap;
    const int64_t key_area = (int64_t)m->content_width - gap_total;
    const int64_t weight_sum = row_weight_sum(row);

    if (key_area <= 0 || weight_sum <= 0) {
        return false;
    }

    /* 整数部分 */
    int64_t assigned = 0;
    for (size_t i = 0; i < row->count; i++) {
        const int64_t quota = key_area * row->keys[i].width_u;
        int32_t width = (int32_t)(quota / weight_sum);
        if (width < 1) {
            width = 1;   /* degenerate weight: a zero sized button is untouchable */
        }
        rects[i].width = width;
        assigned += width;
    }

    /* 剩下的像素发给小数部分最大的键（等权重的键因此一起拿到，宽度保持一致） */
    for (int64_t left = key_area - assigned; left > 0; left--) {
        size_t best = row->count;
        int64_t best_rem = -1;

        for (size_t i = 0; i < row->count; i++) {
            const int64_t quota = key_area * row->keys[i].width_u;
            if (rects[i].width > (int32_t)(quota / weight_sum)) {
                continue;   /* already got its extra pixel */
            }
            const int64_t rem = quota % weight_sum;
            if (rem > best_rem) {
                best_rem = rem;
                best = i;
            }
        }
        if (best >= row->count) {
            break;   /* every key was already bumped: nothing left to hand out */
        }
        rects[best].width++;
    }

    int32_t x = base_x;
    for (size_t i = 0; i < row->count; i++) {
        rects[i].x = x;
        rects[i].y = y;
        rects[i].height = m->row_height;
        x += rects[i].width + m->key_gap;
    }
    return true;
}

/**
 * CENTER row: every key is `width_u` tenths of the standard unit, and the finished
 * row is centered in the grid. This is what keeps the letters the same width on
 * R1/R2/R3 while those rows are naturally indented (they carry 120, 111 and 114
 * tenths respectively) - the "standard phone keyboard" look the old flex layout
 * could not produce, because flex grow re-scaled each row to its own width.
 *
 * ★ 每个键**各自**取整（w = round(width_u * U / 10)），不是"按累计值取整"。
 *   两种取整方式差一像素，但结果完全不同：
 *
 *     累计取整：行严格铺满网格，可是同一个 0.9u 的字母键会按它在行里的位置分别
 *               得到 21 或 22 px —— q=a=22 而 z=21，**同一列的键在不同行不一样宽**。
 *     各自取整：同一个逻辑宽度在任何行、任何位置都是同一个像素宽度（列对得齐），
 *               代价是这一行不严格等于网格宽度，取整余量变成两侧对称的留白
 *               （26 键 R1 在 320 px 屏上是每边 1 px，看不出来）。
 *
 *   指南 §4.2 要的正是后者："先按标准单位计算每个键的宽度，再将整行居中"。
 *
 * ★ 取整方式由 metrics.round_widths 统一决定（整个键盘用同一种）：四舍五入放得下就
 *   四舍五入（键宽最接近设计值），有一个 CENTER 行放不下就全体向下取整（保证同一逻辑
 *   宽度的键在任何行都是同一个像素宽度）。见 center_rows_fit_rounded()。
 */
static bool solve_row_center(const ime_row_t *row, const ime_layout_metrics_t *m, int32_t y,
                             int32_t base_x, ime_key_rect_t *rects)
{
    const int64_t gap_total = (int64_t)(row->count - 1) * m->key_gap;
    const int32_t bias = m->round_widths ? 5000 : 0;

    int64_t row_width = gap_total;
    for (size_t i = 0; i < row->count; i++) {
        int32_t width =
            (int32_t)(((int64_t)row->keys[i].width_u * m->unit_milli + bias) / 10000);
        if (width < 1) {
            width = 1;
        }
        rects[i].width = width;
        row_width += width;
    }

    if (row_width > m->content_width) {
        return false;   /* row wider than the grid: the table is over budget */
    }

    /* Lay the keys out inside the centered row. */
    int32_t x = base_x + (int32_t)((m->content_width - row_width) / 2);
    for (size_t i = 0; i < row->count; i++) {
        rects[i].x = x;
        rects[i].y = y;
        rects[i].height = m->row_height;
        x += rects[i].width + m->key_gap;
    }
    return true;
}

bool ime_layout_solve(const ime_keyboard_t *kb, const ime_layout_metrics_t *m,
                      ime_key_rect_t *rects, size_t cap)
{
    if (kb == NULL || m == NULL || rects == NULL || kb->rows == NULL) {
        return false;
    }
    if (m->row_height <= 0 || m->content_width <= 0 || m->row_count <= 0) {
        return false;
    }
    if (ime_layout_key_count(kb) > cap) {
        return false;
    }

    int32_t y = m->pad_top;

    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];

        if (!row_is_solvable(row)) {
            return false;
        }

        bool ok;
        switch (row->align) {
        case IME_ROW_ALIGN_FILL:
            ok = solve_row_fill(row, m, y, m->x_offset, rects);
            break;
        case IME_ROW_ALIGN_CENTER:
            ok = solve_row_center(row, m, y, m->x_offset, rects);
            break;
        default:
            /* GRID: refuse instead of drawing something wrong. */
            return false;
        }
        if (!ok) {
            return false;
        }

        rects += row->count;
        y += m->row_height + m->row_gap;
    }
    return true;
}

/* -------------------------------------------------------------- validating */

static bool rects_overlap(const ime_key_rect_t *a, const ime_key_rect_t *b)
{
    return (a->x < b->x + b->width) && (b->x < a->x + a->width) &&
           (a->y < b->y + b->height) && (b->y < a->y + a->height);
}

bool ime_layout_validate(const ime_keyboard_t *kb, const ime_layout_metrics_t *m,
                         const ime_key_rect_t *rects, size_t cap,
                         ime_layout_report_t *report)
{
    if (report == NULL) {
        return false;
    }
    memset(report, 0, sizeof(*report));

    if (kb == NULL || m == NULL || rects == NULL || kb->rows == NULL) {
        return false;
    }

    const size_t total = ime_layout_key_count(kb);
    if (total > cap) {
        return false;
    }

    /*
     * Without a definite area height the rows still define one: a key below the
     * last row would be a table/geometry mismatch worth flagging.
     */
    const int32_t area_h = (m->height > 0) ? m->height : m->used_height;

    /* --- size limits (grid level) --- */
    if (m->min_key_width > 0 && m->unit_milli < m->min_key_width * 1000) {
        report->width_violations++;
    }
    if (m->max_key_width > 0 && m->unit_milli > m->max_key_width * 1000) {
        report->width_violations++;
    }
    if (m->row_height < m->min_key_height ||
        (m->max_key_height > 0 && m->row_height > m->max_key_height)) {
        report->height_violations++;
    }

    /* --- per key: bounds, size, gaps --- */
    size_t idx = 0;

    for (size_t r = 0; r < kb->row_count; r++) {
        const ime_row_t *row = &kb->rows[r];
        int32_t prev_right = 0;
        bool have_prev = false;

        for (size_t k = 0; k < row->count; k++) {
            const ime_key_rect_t *rect = &rects[idx];
            idx++;
            report->keys++;

            if (rect->width < 1 || rect->height < 1) {
                report->width_violations++;
            }
            if (rect->x < 0 || rect->y < 0 ||
                rect->x + rect->width > m->width ||
                rect->y + rect->height > area_h) {
                report->out_of_bounds++;
            }
            if (have_prev && (rect->x - prev_right) != m->key_gap) {
                report->bad_gaps++;
            }
            prev_right = rect->x + rect->width;
            have_prev = true;
        }

        /*
         * A CENTER row has to sit symmetrically in the grid; an odd leftover pixel
         * is fine, more means the centering maths and the solver disagree.
         */
        if (row->align == IME_ROW_ALIGN_CENTER && row->count > 0) {
            const ime_key_rect_t *first = &rects[idx - row->count];
            const ime_key_rect_t *last = &rects[idx - 1];
            const int32_t left = first->x - m->x_offset;
            const int32_t right = m->x_offset + m->content_width - (last->x + last->width);
            const int32_t diff = (left > right) ? (left - right) : (right - left);
            if (diff > 1) {
                report->misaligned++;
            }
        }
    }

    /* --- overlap: O(n^2) over at most a few dozen keys, once per rebuild --- */
    for (size_t i = 0; i < total; i++) {
        for (size_t j = i + 1; j < total; j++) {
            if (rects_overlap(&rects[i], &rects[j])) {
                report->overlaps++;
            }
        }
    }

    report->ok = (report->out_of_bounds == 0) && (report->overlaps == 0) &&
                 (report->bad_gaps == 0) && (report->misaligned == 0) &&
                 (report->width_violations == 0) && (report->height_violations == 0);
    return true;
}
