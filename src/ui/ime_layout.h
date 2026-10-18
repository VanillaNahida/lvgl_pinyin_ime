/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 键盘数据模型 + 几何求解引擎：把"键表"变成"每个键的像素矩形"。
 *
 * ★ 这个模块**不依赖 LVGL**（只用到 <stdint.h>/<stdbool.h>/<stddef.h>）：键盘表是纯
 *   数据、布局是纯算术，所以两者都能在 PC 上直接编译、直接断言，不必每次都烧到板子上
 *   才知道"第三行偏了 3 px"（见 test_apps/host_layout_test）。
 *
 * 渲染层（src/ui/lv_pinyin_ime_kb.c）只做两件事：把这里算出来的矩形交给
 * lv_obj_set_pos()/lv_obj_set_size()，以及挂事件回调。几何与交互因此可以各自修改而
 * 互不影响 —— 这一条是旧实现的病根：apply_keyboard() 同时管清理对象、建行、建键、
 * 定宽度和注册事件，任何一处改动都会牵动另一处。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ 键盘数据模型 */

/*
 * Design limits of the default theme. These are macros rather than only struct
 * fields because the rest of the widget has to agree with them: the row height
 * clamp of lv_pinyin_ime_set_row_height() is the same 16..256 px window, and a
 * second copy of those numbers is exactly how the two drift apart.
 */
#define IME_LAYOUT_MIN_KEY_WIDTH  16
#define IME_LAYOUT_MAX_KEY_WIDTH  56
#define IME_LAYOUT_MIN_KEY_HEIGHT 16
#define IME_LAYOUT_MAX_KEY_HEIGHT 256
#define IME_LAYOUT_KEY_GAP        3
#define IME_LAYOUT_ROW_GAP        3

/**
 * Row height used when the caller does not know the keyboard area height (a
 * content-sized widget). 28 keeps a 26-key letter key close to square: on a
 * 320 px screen it comes out about 21 px wide, so the keys are only slightly
 * taller than wide, and four rows plus the candidate bar still fit a 240 px
 * screen. lv_pinyin_ime_set_row_height() overrides it at runtime.
 */
#define IME_LAYOUT_ROW_HEIGHT_DEFAULT 28

/**
 * Upper bound of the keys of one keyboard, i.e. the size of the rectangle buffer
 * a renderer has to provide for ime_layout_solve().
 *
 * The largest table in the component is P3 with 42 keys (4 rows of 10 plus the
 * 2-key bottom row), so 64 leaves room for a few more. host_layout_test asserts
 * every table stays inside it - a keyboard that outgrows the buffer would
 * otherwise only fail to draw at runtime.
 */
#define IME_LAYOUT_KEY_MAX 64

/** What a key does when pressed. */
typedef enum {
    IME_KEY_NONE = 0,
    IME_KEY_LETTER,     /**< ASCII letter, `label` holds it */
    IME_KEY_DIGIT,      /**< 9-key digit group, `label` is "ABC".."WXYZ" */
    IME_KEY_TEXT,       /**< commit `text` verbatim */
    IME_KEY_FULLWIDTH,  /**< commit `label`, full width in Chinese mode */
    IME_KEY_BACKSPACE,
    IME_KEY_ENTER,
    IME_KEY_SPACE,
    IME_KEY_SHIFT,
    IME_KEY_CONFIRM,
    IME_KEY_SEPARATOR,  /**< 分词 / 分隔: insert '\'' */
    IME_KEY_SWITCH_KB,  /**< 26-key <-> 9-key */
    IME_KEY_LANG,       /**< toggle Chinese / English */
    IME_KEY_PANEL,      /**< open the panel named in `text` */
    IME_KEY_T9_PINYIN,  /**< 选拼音: show / hide the 9-key pinyin row */
    IME_KEY_VOICE,      /**< 语音输入: start / stop a recognition session */
} ime_key_action_t;

/**
 * One key of a keyboard row.
 *
 * `width_u` is in tenths of a unit (10 = 1u), but what that unit *means* depends
 * on the row's alignment - see ime_row_align_t. Keep it > 0: a zero width used to
 * be silently treated as 10, which made the row's weight sum disagree with the
 * widths actually handed out.
 */
typedef struct {
    const char *label;
    const char *text;       /**< payload for TEXT / PANEL actions */
    uint8_t width_u;        /**< tenths of a unit, 10 = 1u (see ime_row_align_t) */
    ime_key_action_t action;
} ime_key_t;

/**
 * How a row is laid out horizontally.
 *
 * ★ width_u 在两种模式下的含义**不同**，这是旧实现"同样的 10 单位在不同页面代表不同
 *   尺寸"的根源，现在把它写死在类型上：
 *
 *   IME_ROW_ALIGN_FILL    行内**相对权重**：只和同一行的其它键比大小。整行按权重铺满
 *                         网格宽度（扣除键间距），所以各行的权重之和不必相等
 *                         （20/20/30/25/25 和 10/10 都是合法的）。
 *                         用于底部功能行、符号页的等宽行。
 *
 *   IME_ROW_ALIGN_CENTER  标准键宽的**十分之一**：每个键的左右边缘吸附到"单位位置
 *                         × 标准键宽"的网格上（w = px(前缀+自身) - px(前缀)），
 *                         U 由键盘的参考行反算（见 ime_keyboard_t::ref_row）。
 *                         参考行正好铺满网格宽度、顶住左右边缘；比参考行窄的行
 *                         （26 键第二、三行）整行**居中**，于是天然缩进 —— 这正是
 *                         标准手机键盘的观感，而且同一列的键在不同行严格等宽。
 *
 *   IME_ROW_ALIGN_GRID    预留：按键携带显式逻辑列坐标（跨行对齐用）。目前求解器
 *                         会明确返回 false，而不是静默画歪。
 */
typedef enum {
    IME_ROW_ALIGN_FILL = 0,
    IME_ROW_ALIGN_CENTER,
    IME_ROW_ALIGN_GRID,
} ime_row_align_t;

/** One row of a keyboard. */
typedef struct {
    const ime_key_t *keys;
    size_t count;
    ime_row_align_t align;
} ime_row_t;

/**
 * A complete keyboard.
 *
 * `ref_row` is the index of the row that defines the standard key width U: the
 * row whose keys span the whole grid at the design proportions (26-key R1 with
 * its 120 tenths over 12 keys, the 10-key symbol rows with 100 tenths, ...).
 * U = (grid_width - (count-1) * key_gap) / (weight_sum / 10), so every CENTER
 * row is measured against one and the same unit and the letter keys keep exactly
 * the same width on every row - which is what makes the columns line up.
 */
typedef struct {
    const ime_row_t *rows;
    size_t row_count;
    size_t ref_row;
} ime_keyboard_t;

/* ================================================================ 几何结果 */

/** Final rectangle of one key, relative to the keyboard area's top left. */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} ime_key_rect_t;

/**
 * Design constants of the keyboard geometry.
 *
 * Everything that used to be a per-page magic number (outer margin, key gap, row
 * gap, minimum touch size, voice circle diameter) lives here, so the main
 * keyboard, the symbol panels and the voice page are all held to one spec.
 *
 * ★ 外边距为什么默认是 0：键盘区的坐标原点是控件里那个 kb_rows 容器，而控件 root
 *   自己的样式已经带了 pad_all(2) + pad_row(3)。在这里再减一遍同样的值就是"同一份
 *   间距算两次"——旧代码正好踩了这个坑（update_unit_px() 从父对象宽度里又扣了一次
 *   IME_UI_PAD）。要加也要先把 root 的样式去掉，别两边都留。
 */
typedef struct {
    int32_t outer_pad_x;   /**< left/right margin inside the keyboard area */
    int32_t outer_pad_y;   /**< top/bottom margin inside the keyboard area */
    int32_t key_gap_x;     /**< horizontal space between two keys of a row */
    int32_t key_gap_y;     /**< vertical space between two rows */

    int32_t min_key_width;   /**< floor for the standard 1u key width (touch target) */
    int32_t max_key_width;   /**< ceiling: on a wide screen the grid stops growing */
    int32_t min_key_height;  /**< floor for the row height */
    int32_t max_key_height;  /**< ceiling for the row height */

    int32_t compact_key_gap_x; /**< gaps/pads used when the area is too tight */
    int32_t compact_key_gap_y;
    int32_t compact_pad_x;
    int32_t compact_pad_y;

    int32_t row_height_pref;   /**< row height used when no area height is given */
    int32_t voice_diameter;    /**< preferred diameter of the voice page's circle */
} ime_layout_theme_t;

/**
 * Measured geometry of one keyboard in one area.
 *
 * Fields above the "outputs" line are copied from the theme/arguments so a caller
 * can pass the struct straight to ime_layout_solve() without re-deriving them.
 */
typedef struct {
    /* --- inputs (filled by ime_layout_measure*) --- */
    int32_t width;        /**< area width  (e.g. lv_obj_get_content_width) */
    int32_t height;       /**< area height, <= 0 = "no definite height" */
    int32_t pad_left;
    int32_t pad_right;
    int32_t pad_top;
    int32_t pad_bottom;
    int32_t key_gap;      /**< horizontal gap actually used */
    int32_t row_gap;      /**< vertical gap actually used */
    int32_t min_key_width;
    int32_t max_key_width;
    int32_t min_key_height;
    int32_t max_key_height;
    int32_t ref_units;    /**< reference row width in tenths of a unit (120 = 12u) */
    int32_t ref_keys;     /**< reference row key count */

    /* --- outputs --- */
    int32_t row_count;
    int32_t row_height;
    int32_t unit_milli;    /**< standard key width U in 1/1000 px (fixed point) */
    int32_t x_offset;      /**< left edge of the grid inside the area */
    int32_t content_width; /**< grid width after margins and the max-key cap */
    int32_t used_height;   /**< height actually occupied by the rows */
    bool compact;          /**< compact gaps/pads were needed to fit */
    bool tight;            /**< still below the design minimum: keys got smaller */
} ime_layout_metrics_t;

/** Result of ime_layout_validate(): every counter must be 0 for `ok`. */
typedef struct {
    size_t keys;              /**< rectangles checked */
    size_t out_of_bounds;     /**< x/y < 0, or right/bottom edge past the area */
    size_t overlaps;          /**< a key overlapping another one */
    size_t bad_gaps;          /**< neighbouring keys whose gap != metrics.key_gap */
    size_t misaligned;        /**< a CENTER row whose two margins differ by > 1 px */
    size_t width_violations;  /**< standard key width outside [min,max], or a key < 1 px */
    size_t height_violations; /**< row height outside [min, max] */
    bool ok;
} ime_layout_report_t;

/* ==================================================================== API */

/** Fill @p theme with the component's default design constants. */
void ime_layout_theme_default(ime_layout_theme_t *theme);

/** Number of keys of @p kb (the size of the rect buffer ime_layout_solve() wants). */
size_t ime_layout_key_count(const ime_keyboard_t *kb);

/**
 * Self-check of a keyboard *table* (no geometry involved).
 *
 * Catches the class of bug that used to hide in comments: empty rows, zero
 * widths, a reference row that does not exist, an alignment the solver cannot
 * honour. @p err (optional) receives a human readable reason.
 */
bool ime_layout_validate_keyboard(const ime_keyboard_t *kb, char *err, size_t err_size);

/**
 * Measure a keyboard: available width/height -> standard key width, grid width,
 * row height and the total height the rows will occupy.
 *
 * @param width   area width in px (already excludes any container padding).
 * @param height  area height in px, or <= 0 when the caller does not know it yet
 *                (then theme->row_height_pref decides the row height).
 *
 * Returns false when the area cannot hold a single key (width <= 0, or the gaps
 * alone eat the row).
 */
bool ime_layout_measure(const ime_layout_theme_t *theme, const ime_keyboard_t *kb,
                        int32_t width, int32_t height, ime_layout_metrics_t *out);

/**
 * Same as ime_layout_measure() for a caller that has no keyboard table: the
 * reference grid is described directly as `ref_units` tenths of a unit spread
 * over `ref_keys` keys (12u over 2 keys = 120/2).
 *
 * ★ 语音页就是用它把"和主键盘同一套尺寸规范"落到实处的：同一份 theme、同一个 12u
 *   网格，于是圆形麦克风的直径、底部两个按钮的高度和间距都随主键盘一起缩放，而不是
 *   各写一套固定像素。
 */
bool ime_layout_measure_ref(const ime_layout_theme_t *theme, int32_t width, int32_t height,
                            int32_t ref_units, int32_t ref_keys, ime_layout_metrics_t *out);

/**
 * Solve the rectangles: fill @p rects in row-major key order (rows as declared,
 * keys left to right), exactly ime_layout_key_count(kb) of them.
 *
 * The caller owns the buffer; ime_layout_solve() never allocates, so a keyboard
 * rebuild on the ESP32 does not touch the heap.
 */
bool ime_layout_solve(const ime_keyboard_t *kb, const ime_layout_metrics_t *metrics,
                      ime_key_rect_t *rects, size_t cap);

/**
 * Check the solved rectangles: inside the area, no overlap, equal gaps, centered
 * rows, size limits. Cheap (one pass over the keys) and meant to be called after
 * every solve - a broken table then shows up as a log line instead of a key that
 * silently sits half off screen.
 */
bool ime_layout_validate(const ime_keyboard_t *kb, const ime_layout_metrics_t *metrics,
                         const ime_key_rect_t *rects, size_t cap,
                         ime_layout_report_t *report);

/** Standard key width in whole pixels (fixed point -> px, rounded). */
int32_t ime_layout_unit_px(const ime_layout_metrics_t *metrics);

/** Stable name of an alignment mode, for logs and test output. */
const char *ime_layout_align_name(ime_row_align_t align);

#ifdef __cplusplus
}
#endif
