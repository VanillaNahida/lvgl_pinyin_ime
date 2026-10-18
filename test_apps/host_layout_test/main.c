/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 布局引擎的主机端验证：**只用静态数据**把 ime_layout.c 的几何算清楚，不需要 LVGL、
 * 不需要屏幕、不需要烧板子。
 *
 * 指南把这一条放在阶段 2："实现尺寸测量、标准键宽计算、行居中、填充行分配、行高计算与
 * 矩形校验。先用静态数据验证计算结果。" 于是这里分两层：
 *
 *   1. 合成数据（一定跑）：自造几行键，逐条断言算法本身 —— 标准键宽、行居中、填充
 *      分配、取整、行高、紧凑模式、上限、校验器能抓出什么问题。
 *      编译期只需要 <stdint.h> 那几个头，连 LVGL 都不需要。
 *
 *   2. 真实键表（-DIME_LAYOUT_TEST_KEYMAPS=ON，需要 LVGL 头）：把组件里每一张键盘表
 *      都过一遍求解与校验，在多种分辨率下断言"字母键处处等宽、功能行铺满、居中行对称、
 *      不越界不重叠"，以及每张表的键数没有超过矩形缓冲上限。
 *      这些断言正是以前只能在真机上看出来的东西。
 *
 * 退出码非 0 表示有断言失败。
 */

#include <stdio.h>
#include <string.h>

#include "ui/ime_layout.h"

#ifdef IME_LAYOUT_TEST_KEYMAPS
#include "ui/ime_keymaps.h"
#endif

static int s_checks;
static int s_failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        s_checks++;                                          \
        if (!(cond)) {                                       \
            s_failures++;                                    \
            printf("  [FAIL] ");                             \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

static void heading(const char *title)
{
    printf("\n== %s ==\n", title);
}

/* ------------------------------------------------------------ 合成键表数据 */

/* 26 键 R1 的形状：符(1.5u) + 10 个字母(0.9u) + ⌫(1.5u) = 120 */
static const ime_key_t s_ref_keys[] = {
    {"符", NULL, 15, IME_KEY_PANEL},
    {"q", NULL, 9, IME_KEY_LETTER}, {"w", NULL, 9, IME_KEY_LETTER},
    {"e", NULL, 9, IME_KEY_LETTER}, {"r", NULL, 9, IME_KEY_LETTER},
    {"t", NULL, 9, IME_KEY_LETTER}, {"y", NULL, 9, IME_KEY_LETTER},
    {"u", NULL, 9, IME_KEY_LETTER}, {"i", NULL, 9, IME_KEY_LETTER},
    {"o", NULL, 9, IME_KEY_LETTER}, {"p", NULL, 9, IME_KEY_LETTER},
    {"⌫", NULL, 15, IME_KEY_BACKSPACE},
};

/* R2 的形状：⇧(1.5u) + 9 个字母 + ⏎(1.5u) = 111（居中行，允许比 R1 窄） */
static const ime_key_t s_center_keys[] = {
    {"⇧", NULL, 15, IME_KEY_SHIFT},
    {"a", NULL, 9, IME_KEY_LETTER}, {"s", NULL, 9, IME_KEY_LETTER},
    {"d", NULL, 9, IME_KEY_LETTER}, {"f", NULL, 9, IME_KEY_LETTER},
    {"g", NULL, 9, IME_KEY_LETTER}, {"h", NULL, 9, IME_KEY_LETTER},
    {"j", NULL, 9, IME_KEY_LETTER}, {"k", NULL, 9, IME_KEY_LETTER},
    {"l", NULL, 9, IME_KEY_LETTER},
    {"⏎", NULL, 15, IME_KEY_ENTER},
};

/* 底部功能行：20/20/30/25/25 = 120（FILL：行内比例） */
static const ime_key_t s_fill_keys[] = {
    {"?123", NULL, 20, IME_KEY_PANEL},
    {"⌨", NULL, 20, IME_KEY_SWITCH_KB},
    {"空格", NULL, 30, IME_KEY_SPACE},
    {"中", NULL, 25, IME_KEY_LANG},
    {"✓", NULL, 25, IME_KEY_CONFIRM},
};

static const ime_row_t s_rows[] = {
    {s_ref_keys, sizeof(s_ref_keys) / sizeof(s_ref_keys[0]), IME_ROW_ALIGN_CENTER},
    {s_center_keys, sizeof(s_center_keys) / sizeof(s_center_keys[0]), IME_ROW_ALIGN_CENTER},
    {s_fill_keys, sizeof(s_fill_keys) / sizeof(s_fill_keys[0]), IME_ROW_ALIGN_FILL},
};
static const ime_keyboard_t s_kb = {s_rows, 3, 0};

static bool solve(const ime_keyboard_t *kb, int32_t w, int32_t h, ime_layout_metrics_t *m,
                  ime_key_rect_t *rects, ime_layout_report_t *rep)
{
    ime_layout_theme_t theme;
    ime_layout_theme_default(&theme);

    /* 缓冲区清零：求解只写 ime_layout_key_count() 个矩形，剩下的保持确定值，
     * 这样 memcmp 之类的比较不会读到栈上的垃圾。 */
    memset(rects, 0, sizeof(ime_key_rect_t) * IME_LAYOUT_KEY_MAX);

    if (!ime_layout_measure(&theme, kb, w, h, m)) {
        return false;
    }
    if (!ime_layout_solve(kb, m, rects, IME_LAYOUT_KEY_MAX)) {
        return false;
    }
    if (rep != NULL) {
        (void)ime_layout_validate(kb, m, rects, IME_LAYOUT_KEY_MAX, rep);
    }
    return true;
}

/** 行主序的矩形下标：第 r 行第 k 个键。 */
static size_t rect_index(const ime_keyboard_t *kb, size_t r, size_t k)
{
    size_t idx = k;
    for (size_t i = 0; i < r; i++) {
        idx += kb->rows[i].count;
    }
    return idx;
}

/* ------------------------------------------------------ 合成数据的断言集合 */

static void test_measure_and_unit(void)
{
    heading("标准键宽 / 网格宽度");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];

    /* 320 px 屏：键盘区 316 px（控件内边距已经扣掉），间距 3 px。 */
    CHECK(solve(&s_kb, 316, 0, &m, rects, NULL), "316 px should be solvable");
    CHECK(m.ref_units == 120 && m.ref_keys == 12, "the reference row is R1 (120 tenths/12 keys)");
    /* U = (316 - 11*3) / 12 = 23.583 px */
    CHECK(m.unit_milli == 23583, "unit = (316-33)/12 = 23.583 px, got %d.%03d", m.unit_milli / 1000,
          m.unit_milli % 1000);
    CHECK(m.content_width == 316, "the grid spans the whole area, got %d", (int)m.content_width);
    CHECK(m.x_offset == 0, "nothing to center when the grid fills the area");
    CHECK(m.key_gap == 3 && m.row_gap == 3, "the theme's gaps are used");
    CHECK(m.row_height == IME_LAYOUT_ROW_HEIGHT_DEFAULT, "no area height -> the preferred row height");
    CHECK(!m.compact && !m.tight, "a 320 px screen is neither compact nor tight");

    /* 行高来自键盘区高度：3 行、上下无留白、行间距 3 → (200 - 6) / 3 = 64 */
    CHECK(solve(&s_kb, 316, 200, &m, rects, NULL), "200 px tall should be solvable");
    CHECK(m.row_height == (200 - 2 * 3) / 3, "row height = (H - gaps) / rows = 64, got %d",
          (int)m.row_height);
    CHECK(m.used_height == 3 * m.row_height + 2 * 3, "used height = rows + gaps, got %d",
          (int)m.used_height);
    CHECK(m.used_height <= 200, "the rows fit the area they were measured for");
}

static void test_center_row(void)
{
    heading("CENTER 行：标准键宽 + 吸附单位网格 + 整行居中");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;
    CHECK(solve(&s_kb, 316, 0, &m, rects, &rep), "solvable");
    CHECK(rep.ok, "a valid layout passes its own check");

    /*
     * 边缘吸附：每个键的左右边缘都落在 px(u) = round(u * U / 10) 上（U = 23.583）。
     *   1.5u -> px(15) = 35；1.6u.. 见下。所以 0.9u 的字母是 21 或 22（取决于它的
     *   单位位置），1.5u 的功能键是 35。**同一个单位位置上的键宽度一定相同**。
     */
    CHECK(rects[0].width == 35, "1.5u key at unit 0 = px(15) = 35 px, got %d", (int)rects[0].width);
    CHECK(rects[11].width == 35, "the trailing 1.5u key = 35 px, got %d", (int)rects[11].width);
    for (size_t i = 1; i <= 10; i++) {
        const int w = rects[i].width;
        CHECK(w == 21 || w == 22, "0.9u letter %u is 21 or 22 px, got %d", (unsigned)i, w);
    }

    /* 参考行**一个像素都不剩**：左右边界就是网格边界（这是"两侧不留白"的保证） */
    CHECK(rects[0].x == m.x_offset, "the reference row starts at the grid edge (%d vs %d)",
          (int)rects[0].x, (int)m.x_offset);
    CHECK(rects[11].x + rects[11].width == m.x_offset + m.content_width,
          "and ends exactly at the grid's right edge");
    int total = 0;
    for (size_t i = 0; i < 12; i++) {
        total += rects[i].width;
    }
    CHECK(total + 11 * m.key_gap == m.content_width,
          "its widths + gaps equal the grid width exactly (%d + %d)", total, 11 * m.key_gap);

    /*
     * 同一个单位位置 -> 同一个像素宽度：R2 的第一个字母 a 和 R1 的 q 都在 1.5u 处
     * （符 / ⇧ 都是 1.5u），所以它们必须**完全相等** —— 这就是"列在不同行之间对齐"。
     */
    const ime_key_rect_t *q = &rects[1];
    const ime_key_rect_t *a = &rects[13];
    CHECK(q->width == a->width, "q and a sit at the same unit position, so the same width "
                                "(%d vs %d)", (int)q->width, (int)a->width);
    CHECK(rects[12].width == rects[0].width, "⇧ and 符 are both 1.5u at unit 0 -> equal");

    /* 居中：R2（11 个键）比 R1 窄，两侧留白相等 */
    const ime_key_rect_t *r2_first = &rects[12];
    const ime_key_rect_t *r2_last = &rects[22];
    const int left = r2_first->x - m.x_offset;
    const int right = m.x_offset + m.content_width - (r2_last->x + r2_last->width);
    CHECK(left > 0, "a centered row is indented (%d px)", left);
    CHECK(right - left <= 1 && left - right <= 1, "both margins are equal (%d vs %d)", left, right);
    CHECK(left + (r2_last->x + r2_last->width - r2_first->x) + right == m.content_width,
          "margins + row width = the grid width");

    /* 行与行之间：y 依次加一个行高 + 行间距 */
    CHECK(rects[12].y == rects[0].y + m.row_height + m.row_gap, "rows are one row-height apart");
}

static void test_fill_row(void)
{
    heading("FILL 行：按比例分配，整行铺满");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;
    CHECK(solve(&s_kb, 316, 0, &m, rects, &rep), "solvable");
    CHECK(rep.ok, "a valid layout passes its own check");

    /*
     * 最大余数法：等权重的键**必须等宽**（这是它相对"累计取整"的意义所在），
     * 各自与精确配额最多差 1 px，而且整行（含间距）一个像素都不剩。
     */
    const ime_key_rect_t *f = &rects[23];   /* R4 的五个键 */
    const int gap_total = 4 * 3;
    const int key_area = 316 - gap_total;
    CHECK(f[0].x == m.x_offset, "a filled row starts at the grid's left edge");
    CHECK(f[4].x + f[4].width == m.x_offset + m.content_width,
          "a filled row ends exactly at the grid's right edge (no stray pixel)");
    CHECK(f[0].width == f[1].width, "two 20%% keys come out equal (%d vs %d)", (int)f[0].width,
          (int)f[1].width);
    CHECK(f[3].width == f[4].width, "two 25%% keys come out equal (%d vs %d)", (int)f[3].width,
          (int)f[4].width);
    for (int i = 0; i < 5; i++) {
        const int quota = key_area * s_fill_keys[i].width_u / 120;
        const int d = f[i].width - quota;
        CHECK(d >= 0 && d <= 1, "key %d is its exact quota (%d) or one pixel more (%d)", i, quota,
              (int)f[i].width);
    }
    CHECK(f[2].width > f[3].width, "the 30%% space bar is wider than a 25%% key");

    /* 行的总宽（含间距）必须**正好**等于网格宽度：累计分配不能留缝 */
    int total = 0;
    for (int i = 0; i < 5; i++) {
        total += f[i].width;
    }
    CHECK(total + gap_total == m.content_width, "the widths + gaps add up to the grid (%d + %d)",
          total, gap_total);

    /* 间距处处一致 */
    for (int i = 1; i < 5; i++) {
        CHECK(f[i].x - (f[i - 1].x + f[i - 1].width) == m.key_gap, "the gap before key %d is %d px",
              i, (int)m.key_gap);
    }
}

static void test_width_limits(void)
{
    heading("尺寸边界：最小键宽 / 紧凑间距 / 最大键宽");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;

    /* 极窄：240 px。U = (236-33)/12 = 16.9 px，刚好在 16 px 触控下界之上 */
    CHECK(solve(&s_kb, 236, 0, &m, rects, &rep), "236 px is still solvable");
    CHECK(!m.tight, "U = 16.9 px is above the 16 px floor");
    CHECK(m.unit_milli >= 16 * 1000, "the unit honors the touch floor");
    CHECK(!m.compact, "and it did not need the compact gaps");

    /*
     * 再窄一档（220 px）：正常间距下 U = 15.6 px 会跌破下界，但缩掉间距与留白之后
     * U = 16.3 px 又能达标 —— 这正是紧凑模式存在的意义：**先挤留白，别挤按键**。
     */
    CHECK(solve(&s_kb, 220, 0, &m, rects, &rep), "220 px is still solvable");
    CHECK(m.compact, "the compact gaps kick in before the keys go below the floor");
    CHECK(m.key_gap == 2, "the compact key gap is 2 px, got %d", (int)m.key_gap);
    CHECK(m.unit_milli >= 16 * 1000, "and the keys are back above the touch floor");
    CHECK(!m.tight, "nothing had to shrink below the design minimum");

    /* 再窄到怎么挤都不够：如实报 tight，但绝不重叠、绝不越界 */
    CHECK(solve(&s_kb, 180, 0, &m, rects, &rep), "180 px is still solvable");
    CHECK(m.tight, "the layout says it had to shrink below the design minimum");
    CHECK(rep.width_violations > 0, "the checker reports the too-narrow keys");
    CHECK(rep.out_of_bounds == 0 && rep.overlaps == 0, "but nothing overlaps or leaves the area");
    CHECK(rep.bad_gaps == 0, "and the gaps are still even");

    /* 大屏：键宽到上限就不再长，多出来的空间变成两侧留白，网格居中 */
    CHECK(solve(&s_kb, 900, 0, &m, rects, &rep), "900 px is solvable");
    CHECK(m.unit_milli == IME_LAYOUT_MAX_KEY_WIDTH * 1000, "the unit is capped at %d px",
          IME_LAYOUT_MAX_KEY_WIDTH);
    CHECK(m.content_width < 900, "the grid stops growing (%d px)", (int)m.content_width);
    CHECK(m.x_offset > 0, "and it is centered (%d px on the left)", (int)m.x_offset);
    CHECK(m.x_offset * 2 + m.content_width <= 900, "the grid plus both margins fits the area");
    CHECK(rep.width_violations == 0, "hitting the ceiling is not a violation");

    /* 放不下：间距就吃掉了整行 -> 明确失败，而不是画出一堆 0 宽的键 */
    ime_layout_theme_t theme;
    ime_layout_theme_default(&theme);
    CHECK(!ime_layout_measure(&theme, &s_kb, 8, 0, &m), "8 px cannot hold 12 keys: measure fails");
    CHECK(!ime_layout_measure(&theme, &s_kb, 0, 0, &m), "a zero width area fails too");
}

static void test_height_limits(void)
{
    heading("行高：由键盘区高度反推，最小触控高度兜底");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];

    /* 空间充足 */
    CHECK(solve(&s_kb, 316, 400, &m, rects, NULL), "solvable");
    CHECK(m.row_height == (400 - 2 * 3) / 3, "row height = (H - gaps) / rows, got %d",
          (int)m.row_height);
    CHECK(!m.tight, "400 px for 3 rows is comfortable");

    /* 空间不足：先缩间距，再守最小触控高度并把 tight 报出来 */
    CHECK(solve(&s_kb, 316, 40, &m, rects, NULL), "40 px is solvable (clamped)");
    CHECK(m.row_height == IME_LAYOUT_MIN_KEY_HEIGHT, "the row height stops at %d px, got %d",
          IME_LAYOUT_MIN_KEY_HEIGHT, (int)m.row_height);
    CHECK(m.tight, "and the layout reports that it did not fit");
    CHECK(m.used_height > 40, "used height honestly exceeds the area");

    /* 上限：超高的键盘区不会让一行变成整屏 */
    CHECK(solve(&s_kb, 316, 4000, &m, rects, NULL), "solvable");
    CHECK(m.row_height <= IME_LAYOUT_MAX_KEY_HEIGHT, "the row height is capped at %d px",
          IME_LAYOUT_MAX_KEY_HEIGHT);
}

static void test_validate_catches_problems(void)
{
    heading("校验器：越界 / 重叠 / 间距 / 居中 / 尺寸");

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;
    CHECK(solve(&s_kb, 316, 0, &m, rects, &rep), "solvable");
    CHECK(rep.ok && rep.keys == 28, "28 keys, all checks green (keys=%u)", (unsigned)rep.keys);

    /* 越界：把一个键推到网格外面（它自己就探出去了，未必压到邻居） */
    ime_key_rect_t broken[IME_LAYOUT_KEY_MAX];
    memcpy(broken, rects, sizeof(broken));
    broken[0].x = m.width - 1;
    CHECK(ime_layout_validate(&s_kb, &m, broken, IME_LAYOUT_KEY_MAX, &rep) && !rep.ok,
          "a key outside the area is reported");
    CHECK(rep.out_of_bounds > 0, "it shows up as out of bounds");

    /* 重叠：把第二个键搬到第一个键上面 */
    memcpy(broken, rects, sizeof(broken));
    broken[1].x = broken[0].x + 1;
    CHECK(ime_layout_validate(&s_kb, &m, broken, IME_LAYOUT_KEY_MAX, &rep) &&
              rep.overlaps > 0,
          "two keys on top of each other are reported");

    /* 间距：把某个键挪 1 px */
    memcpy(broken, rects, sizeof(broken));
    broken[3].x += 1;
    CHECK(ime_layout_validate(&s_kb, &m, broken, IME_LAYOUT_KEY_MAX, &rep) &&
              rep.bad_gaps > 0,
          "a wrong gap is reported");

    /* 居中：把整行往右挪 4 px */
    memcpy(broken, rects, sizeof(broken));
    for (size_t i = 12; i < 23; i++) {
        broken[i].x += 4;
    }
    CHECK(ime_layout_validate(&s_kb, &m, broken, IME_LAYOUT_KEY_MAX, &rep) &&
              rep.misaligned > 0,
          "an off-center CENTER row is reported");

    /* 尺寸：行高低于下界 */
    ime_layout_metrics_t narrow = m;
    narrow.row_height = IME_LAYOUT_MIN_KEY_HEIGHT - 1;
    CHECK(ime_layout_validate(&s_kb, &narrow, rects, IME_LAYOUT_KEY_MAX, &rep) &&
              rep.height_violations > 0,
          "a row height below the touch floor is reported");
}

static void test_bad_tables(void)
{
    heading("键表自检：0 宽度 / 空行 / ref_row / 未实现的对齐");

    char err[64];
    CHECK(ime_layout_validate_keyboard(&s_kb, err, sizeof(err)), "the synthetic table is valid");

    /* width_u = 0：以前被当成 10 兜底，于是权重之和与实际分配对不上 */
    ime_key_t zero_width[2] = {{"a", NULL, 0, IME_KEY_LETTER}, {"b", NULL, 10, IME_KEY_LETTER}};
    const ime_row_t zero_row[1] = {{zero_width, 2, IME_ROW_ALIGN_FILL}};
    const ime_keyboard_t zero_kb = {zero_row, 1, 0};
    CHECK(!ime_layout_validate_keyboard(&zero_kb, err, sizeof(err)), "a zero width key is rejected");
    printf("  (reason: %s)\n", err);

    /* 空行 */
    const ime_row_t empty_row[1] = {{NULL, 0, IME_ROW_ALIGN_FILL}};
    const ime_keyboard_t empty_kb = {empty_row, 1, 0};
    CHECK(!ime_layout_validate_keyboard(&empty_kb, err, sizeof(err)), "an empty row is rejected");

    /* ref_row 越界 */
    ime_keyboard_t bad_ref = s_kb;
    bad_ref.ref_row = 7;
    CHECK(!ime_layout_validate_keyboard(&bad_ref, err, sizeof(err)), "a bad ref_row is rejected");

    /* GRID：还没实现，就必须明确拒绝（而不是静默画歪） */
    ime_row_t grid_row[3];
    memcpy(grid_row, s_rows, sizeof(grid_row));
    grid_row[1].align = IME_ROW_ALIGN_GRID;
    const ime_keyboard_t grid_kb = {grid_row, 3, 0};
    CHECK(!ime_layout_validate_keyboard(&grid_kb, err, sizeof(err)),
          "an unimplemented alignment is rejected");
    printf("  (reason: %s)\n", err);

    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    CHECK(!solve(&grid_kb, 316, 0, &m, rects, NULL), "and the solver refuses to draw it");

    /* 字母键没有键面文字：按下去不知道该输入什么 */
    ime_key_t no_label[2] = {{"", NULL, 10, IME_KEY_LETTER}, {"b", NULL, 10, IME_KEY_LETTER}};
    const ime_row_t nl_row[1] = {{no_label, 2, IME_ROW_ALIGN_FILL}};
    const ime_keyboard_t nl_kb = {nl_row, 1, 0};
    CHECK(!ime_layout_validate_keyboard(&nl_kb, err, sizeof(err)),
          "a letter key without a label is rejected");
}

static void test_deterministic_and_capacity(void)
{
    heading("可重复性与缓冲上限");

    ime_layout_metrics_t m1, m2;
    ime_key_rect_t r1[IME_LAYOUT_KEY_MAX], r2[IME_LAYOUT_KEY_MAX];
    CHECK(solve(&s_kb, 316, 200, &m1, r1, NULL) && solve(&s_kb, 316, 200, &m2, r2, NULL),
          "solvable twice");
    CHECK(memcmp(&m1, &m2, sizeof(m1)) == 0, "the same input measures the same");
    CHECK(memcmp(r1, r2, sizeof(r1)) == 0, "and solves to the same rectangles (no hidden state)");

    /* 缓冲区不够时必须失败，而不是写越界 */
    CHECK(!ime_layout_solve(&s_kb, &m1, r1, 5), "a too small buffer is refused");
    CHECK(ime_layout_key_count(&s_kb) == 28, "the key count is 28, got %u",
          (unsigned)ime_layout_key_count(&s_kb));
    CHECK(ime_layout_align_name(IME_ROW_ALIGN_FILL)[0] == 'F', "alignment names are stable");
    CHECK(ime_layout_align_name(IME_ROW_ALIGN_CENTER)[0] == 'C', "alignment names are stable");
    CHECK(strcmp(ime_layout_align_name(IME_ROW_ALIGN_GRID), "GRID") == 0, "GRID is named");
}

/* ------------------------------------------------------------ 真实键表扫描 */

#ifdef IME_LAYOUT_TEST_KEYMAPS
/* 典型屏幕宽度：240(小) 320(常见) 480(大) 800(需要触发键宽上限) */
static const int32_t s_widths[] = {240, 272, 320, 360, 480, 640, 800};
/* 键盘区高度：0 = 未定（用首选行高），其余是 fit 模式下的实际可用高度 */
static const int32_t s_heights[] = {0, 96, 150, 200, 300};

static void test_real_tables(void)
{
    heading("组件里每一张键表 × 多种分辨率");

    CHECK(ime_keymaps_count >= 7, "all keyboards are registered (%u)", (unsigned)ime_keymaps_count);

    for (size_t k = 0; k < ime_keymaps_count; k++) {
        const ime_keymap_entry_t *entry = &ime_keymaps[k];
        const ime_keyboard_t *kb = entry->kb;

        char err[64];
        CHECK(ime_layout_validate_keyboard(kb, err, sizeof(err)), "%s: table is valid (%s)",
              entry->name, err);
        const size_t keys = ime_layout_key_count(kb);
        CHECK(keys > 0 && keys <= IME_LAYOUT_KEY_MAX, "%s: %u keys fit the rect buffer (%d)",
              entry->name, (unsigned)keys, IME_LAYOUT_KEY_MAX);

        for (size_t wi = 0; wi < sizeof(s_widths) / sizeof(s_widths[0]); wi++) {
            for (size_t hi = 0; hi < sizeof(s_heights) / sizeof(s_heights[0]); hi++) {
                const int32_t w = s_widths[wi];
                /*
                 * 键盘区宽度 = 屏幕宽度：控件 root 的左右内边距是 0（键盘要顶到屏幕
                 * 左右边缘，见 IME_STYLE_ROOT_PAD_X）。上下那 2 px 属于垂直预算，
                 * 由 area_h 之外的调用方负责扣。
                 */
                const int32_t area_w = w;
                const int32_t area_h = s_heights[hi];

                ime_layout_theme_t theme;
                ime_layout_theme_default(&theme);
                ime_layout_metrics_t m;
                ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
                ime_layout_report_t rep;

                if (!ime_layout_measure(&theme, kb, area_w, area_h, &m)) {
                    /* 240 px 宽只可能"放不下"到某一张表都不剩；其余都不该失败 */
                    CHECK(w <= 240, "%s: %d px wide cannot be measured", entry->name, (int)w);
                    continue;
                }
                CHECK(ime_layout_solve(kb, &m, rects, IME_LAYOUT_KEY_MAX),
                      "%s: solvable at %dx%d", entry->name, (int)area_w, (int)area_h);
                (void)ime_layout_validate(kb, &m, rects, IME_LAYOUT_KEY_MAX, &rep);

                if (!rep.ok) {
                    printf("  [FAIL] %s at %dx%d: oob=%u overlap=%u gap=%u align=%u w=%u h=%u\n",
                           entry->name, (int)area_w, (int)area_h, (unsigned)rep.out_of_bounds,
                           (unsigned)rep.overlaps, (unsigned)rep.bad_gaps,
                           (unsigned)rep.misaligned, (unsigned)rep.width_violations,
                           (unsigned)rep.height_violations);
                    s_failures++;
                }
                s_checks++;

                /*
                 * 判据（用户报的"最左侧和最右侧均有留白"）：**整行宽度等于网格宽度
                 * 的行，必须顶住网格的左右边缘**。参考行（26 键 R1、九键四行、符号页
                 * 的十列行）和所有 FILL 行都属于这一类 —— 它们的单位之和与参考行相同
                 * （或按定义铺满），所以不该留下任何边距。
                 */
                for (size_t r = 0; r < kb->row_count; r++) {
                    const ime_row_t *row = &kb->rows[r];
                    if (row->keys == NULL || row->count == 0) {
                        continue;
                    }
                    int64_t units = 0;
                    for (size_t k = 0; k < row->count; k++) {
                        units += row->keys[k].width_u;
                    }
                    const ime_key_rect_t *first = &rects[rect_index(kb, r, 0)];
                    const ime_key_rect_t *last = &rects[rect_index(kb, r, row->count - 1)];
                    const bool full_width =
                        (row->align == IME_ROW_ALIGN_FILL) || (units == m.ref_units);
                    if (!full_width) {
                        continue;
                    }
                    const int32_t left = first->x - m.x_offset;
                    const int32_t right =
                        m.x_offset + m.content_width - (last->x + last->width);
                    if (left != 0 || right != 0) {
                        printf("  [FAIL] %s at %dx%d: row %u is a full-width row but leaves "
                               "%d px on the left and %d px on the right\n",
                               entry->name, (int)area_w, (int)area_h, (unsigned)r, (int)left,
                               (int)right);
                        s_failures++;
                    }
                    s_checks++;
                }

                /*
                 * 判据（指南 §11 的"字母键比例正确"）：**同一个单位位置**上的键，
                 * 在任何两行之间都必须是同一个像素宽度 —— 这就是"列对齐"。
                 * 单位位置 = 该键左边缘距离行首的十分之一单位数；26 键 R1 的 q 与
                 * R2 的 a 都在 1.5u 处，九键四行的第 k 列都在同一个位置。
                 *
                 * 只有**位置相同**才要求完全相等：同一行里相邻的等宽键差 1 px 是
                 * "整行精确铺满"的必然代价（21/22 交替），并排看不出来。
                 */
                for (size_t r1 = 0; r1 < kb->row_count; r1++) {
                    if (kb->rows[r1].align != IME_ROW_ALIGN_CENTER) {
                        continue;
                    }
                    for (size_t r2 = r1 + 1; r2 < kb->row_count; r2++) {
                        if (kb->rows[r2].align != IME_ROW_ALIGN_CENTER) {
                            continue;
                        }
                        int32_t pos1 = 0;
                        for (size_t k1 = 0; k1 < kb->rows[r1].count; k1++, pos1 += kb->rows[r1].keys[k1].width_u) {
                            int32_t pos2 = 0;
                            for (size_t k2 = 0; k2 < kb->rows[r2].count; k2++, pos2 += kb->rows[r2].keys[k2].width_u) {
                                if (pos1 != pos2) {
                                    continue;
                                }
                                const ime_key_rect_t *ra = &rects[rect_index(kb, r1, k1)];
                                const ime_key_rect_t *rb = &rects[rect_index(kb, r2, k2)];
                                if (ra->width != rb->width) {
                                    printf("  [FAIL] %s at %dx%d: unit position %.1fu is %d px on "
                                           "row %u but %d px on row %u\n",
                                           entry->name, (int)area_w, (int)area_h, pos1 / 10.0,
                                           (int)ra->width, (unsigned)r1, (int)rb->width,
                                           (unsigned)r2);
                                    s_failures++;
                                }
                                s_checks++;
                            }
                        }
                    }
                }
            }
        }
    }
}

/** 26 键：字母键在 R1/R2/R3 上等宽；R2/R3 居中缩进；R4 铺满。 */
static void test_k26_shape(void)
{
    heading("26 键的形状（列对齐 / 缩进 / 底行铺满）");

    ime_layout_theme_t theme;
    ime_layout_theme_default(&theme);
    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;

    CHECK(ime_layout_measure(&theme, &ime_kb_k26_cn, 316, 0, &m), "k26 measures at 316 px");
    CHECK(ime_layout_solve(&ime_kb_k26_cn, &m, rects, IME_LAYOUT_KEY_MAX), "k26 solves");
    CHECK(ime_layout_validate(&ime_kb_k26_cn, &m, rects, IME_LAYOUT_KEY_MAX, &rep) && rep.ok,
          "k26 passes its own check");

    /*
     * **同一个单位位置**上的键宽度必须完全相等，这才是"列对齐"：
     *   q（R1，1.5u 的 符 之后）与 a（R2，1.5u 的 ⇧ 之后）都在 1.5u 处；
     *   z（R3，1.5u 的分词 + 0.9u 的 - 之后 = 2.4u 处）与 w（R1，2.4u 处）同位置。
     */
    const ime_key_rect_t *q = &rects[1];
    const ime_key_rect_t *a = &rects[13];
    const ime_key_rect_t *w = &rects[2];
    const ime_key_rect_t *z = &rects[26];
    CHECK(q->width == a->width, "q and a are both at 1.5u -> the same width (%d vs %d)",
          (int)q->width, (int)a->width);
    CHECK(w->width == z->width, "w and z are both at 2.4u -> the same width (%d vs %d)",
          (int)w->width, (int)z->width);
    CHECK(q->height == a->height && a->height == z->height, "and the same height");

    /* R2 / R3 都是居中行：整体比 R1 窄，且两侧留白相等 */
    const int r1_l = rects[0].x;
    const int r1_r = rects[11].x + rects[11].width;
    const int r2_l = rects[12].x;
    const int r2_r = rects[22].x + rects[22].width;
    const int r3_l = rects[23].x;
    const int r3_r = rects[34].x + rects[34].width;
    CHECK(r1_l == m.x_offset && r1_r == m.x_offset + m.content_width,
          "R1 spans the grid exactly: %d..%d in %d..%d", r1_l, r1_r, (int)m.x_offset,
          (int)(m.x_offset + m.content_width));
    CHECK(r2_l > r1_l && r2_r < r1_r, "R2 is indented on both sides (%d..%d vs %d..%d)", r2_l, r2_r,
          r1_l, r1_r);
    CHECK(r3_l > r1_l && r3_r < r1_r, "R3 is indented on both sides (%d..%d)", r3_l, r3_r);
    CHECK((r2_l - r1_l) - (r1_r - r2_r) <= 1 && (r1_r - r2_r) - (r2_l - r1_l) <= 1,
          "R2's two margins are equal");
    CHECK((r3_l - r1_l) - (r1_r - r3_r) <= 1 && (r1_r - r3_r) - (r3_l - r1_l) <= 1,
          "R3's two margins are equal");

    /* R4 是 FILL：必须正好铺满网格 */
    const ime_key_rect_t *r4 = &rects[35];
    CHECK(r4[0].x == m.x_offset, "the bottom row starts at the grid edge");
    CHECK(r4[4].x + r4[4].width == m.x_offset + m.content_width,
          "the bottom row ends at the grid edge");
    /* 12u 的底行：空格最大、两个角键一样宽 */
    CHECK(r4[0].width == r4[1].width, "the two 2u corner keys are equal");
    CHECK(r4[3].width == r4[4].width, "the 2.5u keys are equal");
    CHECK(r4[2].width > r4[3].width, "the 3u space bar is wider than a 2.5u key");
}

/** 9 键：四行都正好铺满（5 个 2.4u 的键 = 12u），空格占 60%。 */
static void test_k9_shape(void)
{
    heading("九键的形状（等宽列 / 空格比例）");

    ime_layout_theme_t theme;
    ime_layout_theme_default(&theme);
    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;

    CHECK(ime_layout_measure(&theme, &ime_kb_k9, 316, 0, &m), "k9 measures");
    CHECK(ime_layout_solve(&ime_kb_k9, &m, rects, IME_LAYOUT_KEY_MAX), "k9 solves");
    CHECK(ime_layout_validate(&ime_kb_k9, &m, rects, IME_LAYOUT_KEY_MAX, &rep) && rep.ok,
          "k9 passes its own check");

    /*
     * 四行都是 CENTER 行（5 个 2.4u 的键 = 12u），单位位置逐列相同 —— 所以四行必须
     * **完全对齐**：同一列在任何一行都是同一个 x 和同一个宽度。
     * ★ 行内相邻的键可能差 1 px（124/125 交替）：那是"整行正好 12u、一个像素不剩"的
     *   代价，四行之间仍然完全一致。
     */
    for (size_t r = 0; r < 3; r++) {
        const ime_key_rect_t *row = &rects[r * 5];
        for (size_t k = 0; k < 5; k++) {
            const ime_key_rect_t *first = &rects[k];
            CHECK(row[k].x == first->x, "row %u column %u starts where row 0 does (dx=%d)",
                  (unsigned)r, (unsigned)k, (int)(row[k].x - first->x));
            CHECK(row[k].width == first->width, "row %u column %u has row 0's width (%d vs %d)",
                  (unsigned)r, (unsigned)k, (int)row[k].width, (int)first->width);
            if (k > 0) {
                CHECK(row[k].x - (row[k - 1].x + row[k - 1].width) == m.key_gap, "even gaps");
            }
        }
        /* 每行都正好铺满网格（5 个 2.4u = 12u） */
        CHECK(row[0].x == m.x_offset, "row %u starts at the grid edge", (unsigned)r);
        CHECK(row[4].x + row[4].width == m.x_offset + m.content_width,
              "row %u ends at the grid edge", (unsigned)r);
    }

    /* 底行 FILL：20/20/60 */
    const ime_key_rect_t *r4 = &rects[15];
    const int area = m.content_width - 2 * m.key_gap;
    CHECK(r4[2].width == area * 60 / 100, "the space bar is 60%% of the row: %d vs %d",
          (int)r4[2].width, area * 60 / 100);
    CHECK(r4[0].width == r4[1].width, "the two 20%% keys are equal");
    CHECK(r4[2].x + r4[2].width == m.x_offset + m.content_width, "the row is filled");
}

/** 符号面板：10 列等宽；P2 是三列，但左右边界与行高与别的面板一致。 */
static void test_panel_shape(void)
{
    heading("符号面板（10 列等宽 / P2 三列 / 左右边界一致）");

    ime_layout_theme_t theme;
    ime_layout_theme_default(&theme);
    ime_layout_metrics_t m;
    ime_key_rect_t rects[IME_LAYOUT_KEY_MAX];
    ime_layout_report_t rep;

    const ime_keyboard_t *panels[] = {&ime_kb_p1, &ime_kb_p1_alt, &ime_kb_p3};
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        CHECK(ime_layout_measure(&theme, panels[i], 316, 200, &m), "panel %u measures", (unsigned)i);
        CHECK(ime_layout_solve(panels[i], &m, rects, IME_LAYOUT_KEY_MAX), "panel %u solves",
              (unsigned)i);
        CHECK(ime_layout_validate(panels[i], &m, rects, IME_LAYOUT_KEY_MAX, &rep) && rep.ok,
              "panel %u passes its own check", (unsigned)i);

        const ime_key_rect_t *row1 = &rects[0];
        CHECK(row1[0].x == m.x_offset, "panel %u row 1 starts at the grid edge", (unsigned)i);
        CHECK(row1[9].x + row1[9].width == m.x_offset + m.content_width,
              "panel %u row 1 fills the grid", (unsigned)i);
        for (size_t k = 1; k < 10; k++) {
            const int d = row1[k].width - row1[0].width;
            CHECK(d <= 1 && d >= -1, "panel %u column %u is the same width (%d vs %d)",
                  (unsigned)i, (unsigned)k, (int)row1[k].width, (int)row1[0].width);
        }
    }

    /* P2：三列，四行都是同一套列宽 */
    CHECK(ime_layout_measure(&theme, &ime_kb_p2, 316, 200, &m), "p2 measures");
    CHECK(ime_layout_solve(&ime_kb_p2, &m, rects, IME_LAYOUT_KEY_MAX), "p2 solves");
    for (size_t r = 0; r < 4; r++) {
        const ime_key_rect_t *row = &rects[r * 3];
        for (size_t k = 0; k < 3; k++) {
            const int d = row[k].width - rects[0].width;
            CHECK(d <= 1 && d >= -1, "p2 row %u column %u keeps the column width (%d vs %d)",
                  (unsigned)r, (unsigned)k, (int)row[k].width, (int)rects[0].width);
            const int dx = row[k].x - rects[k].x;
            CHECK(dx == 0, "p2 row %u column %u is aligned with row 1 (dx=%d)", (unsigned)r,
                  (unsigned)k, dx);
        }
    }
}
#endif /* IME_LAYOUT_TEST_KEYMAPS */

int main(void)
{
    printf("ime_layout host test: %s\n",
#ifdef IME_LAYOUT_TEST_KEYMAPS
           "synthetic data + the component's real key tables"
#else
           "synthetic data only (build with -DIME_LAYOUT_TEST_KEYMAPS=ON for the real tables)"
#endif
    );

    test_measure_and_unit();
    test_center_row();
    test_fill_row();
    test_width_limits();
    test_height_limits();
    test_validate_catches_problems();
    test_bad_tables();
    test_deterministic_and_capacity();

#ifdef IME_LAYOUT_TEST_KEYMAPS
    test_real_tables();
    test_k26_shape();
    test_k9_shape();
    test_panel_shape();
#endif

    printf("\n%d checks, %d failure(s)\n", s_checks, s_failures);
    return (s_failures == 0) ? 0 : 1;
}
