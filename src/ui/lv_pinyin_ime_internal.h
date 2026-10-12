/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private state shared by the UI translation units. Nothing here is part of
 * the public API: include/lvgl_pinyin_ime/ stays free of internals.
 */

#pragma once

#include "lvgl.h"

#include "core/ime_session.h"
#include "ime_layout.h"     /* ime_key_t / ime_row_t / ime_keyboard_t + the geometry solver */
#include "lvgl_pinyin_ime/lv_pinyin_ime_types.h"
#include "lv_pinyin_ime_style.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Default height of one key row / candidate row in pixels.
 *
 * The value lives in ime_layout.h because the layout engine's theme carries it
 * too (theme->row_height_pref); two copies of "28" is how the row height and the
 * layout's idea of the row height drift apart.
 *
 * The height actually used at build time is IME_UI_ROW_HEIGHT_PX, which
 * lv_pinyin_ime_set_row_height() overrides. A large panel wants taller rows:
 * with the default, the 5-row 26-key layout is only about 150 px tall, which on
 * a 480 px screen leaves the keys small and hard to hit.
 */
#define IME_UI_ROW_HEIGHT_DEFAULT IME_LAYOUT_ROW_HEIGHT_DEFAULT

/**
 * Margin applied by the widget root's own style (see ime_style_apply_root) and
 * the gap between the root's children.
 *
 * ★ 键盘区的坐标原点是 ctx->kb_rows —— 它在 root 的内容区里，root 的 pad_all 已经
 *   让出了这两个像素。所以布局引擎的 outer_pad_* 必须是 0：同一份间距算两次，正是
 *   旧 update_unit_px() 从父对象宽度里再扣一次 IME_UI_PAD 时的毛病。
 */
#define IME_UI_PAD IME_STYLE_ROOT_PAD

/** Candidates drawn at once in the candidate bar (one page). */
#define IME_UI_CAND_MAX 12

/**
 * Upper bound of the keys of one keyboard (the static rectangle buffer).
 *
 * The value lives in ime_layout.h: it is the capacity ime_layout_solve() needs,
 * not a UI detail, and host_layout_test asserts every table fits in it.
 */
#define IME_UI_KEY_MAX IME_LAYOUT_KEY_MAX

/**
 * 语音预览快照缓冲的长度（"这段预览之前输入框长什么样"）。
 *
 * 只用来做前缀比对，不需要装下整段用户输入：输入框内容比它长时，比对退化成
 * "前 320 字节是否一致"，够用且省内存（这段在 ctx 里，ctx 是堆上的）。
 */
#define IME_VOICE_BASE_MAX 320

/**
 * "上一次写进输入框的文本"缓冲的长度，用于预览去重。
 *
 * 故意用 UI 自己的常量而不是语音协议层的 IME_VOICE_TRANSCRIPT_MAX：后者定义在
 * src/voice/ime_voice_proto.h，而那个头只在 CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y 时
 * 才被包含进来 —— 拿它做数组维度会让"语音关"的配置编不过（踩过）。
 */
#define IME_VOICE_PREVIEW_MAX 640

/** Pinyin chips drawn at once in the 9-key pinyin row. */
#define IME_UI_T9_PINYIN_MAX 8

/* ------------------------------------------------- backspace hold + gesture */

/**
 * Caption of the hold-to-clear hint.
 *
 * Shown while the backspace key is held down; sliding the finger left by about
 * the width of the pill (that is, reaching its far end) clears the text area.
 * Every character here is inside the IME font subset (GB2312 + UI symbols), so
 * the hint never renders as boxes - if you change the text, check it against
 * generated/charset.txt first (tools/check_charset.py).
 */
#define IME_UI_BKSP_HINT_TEXT "← 左滑清空输入框"

/** Lower bound of the slide distance that clears the text area (px). */
#define IME_UI_BKSP_CLEAR_MIN 72

/** Upper bound of that distance: on a wide screen the pill must not demand a
 *  slide across half the keyboard. */
#define IME_UI_BKSP_CLEAR_MAX 240

/** Width of the layout unit `u` in pixels, for the active screen. */
extern int IME_UI_UNIT_PX;

/**
 * Height of one key / candidate row in pixels, for the active screen.
 *
 * Initialised to IME_UI_ROW_HEIGHT_DEFAULT; lv_pinyin_ime_set_row_height()
 * changes it. Read when a row is created, so it has to be set before
 * lv_pinyin_ime_create().
 */
extern int IME_UI_ROW_HEIGHT_PX;

/**
 * Target total height of the whole widget in pixels (0 = fitting disabled).
 *
 * Set by lv_pinyin_ime_set_fit_height(). When non-zero, ime_ui_apply_sizes()
 * derives IME_UI_ROW_HEIGHT_PX from this and the *visible* row count, and
 * re-applies it to every row, so the keyboard fills the target height no matter
 * how many rows the current mode / panel needs (see ime_ui_apply_sizes).
 */
extern int IME_UI_FIT_HEIGHT_PX;

/**
 * The key data model (ime_key_action_t, ime_key_t, ime_row_t, ime_keyboard_t)
 * moved to ime_layout.h: it is the layout engine's input, and the engine has to
 * build on a host *without* LVGL. This header includes it, so nothing else has
 * to change.
 */

#ifndef IME_UI_PINYIN_TEXT_MAX
#define IME_UI_PINYIN_TEXT_MAX 96
#endif

typedef struct {
    lv_obj_t *obj;    /**< the IME root object */
    lv_obj_t *ta;     /**< bound text area, may be NULL */

    /*
     * "Click outside to dismiss" (2026-10): the root(s) we attached the dismiss
     * callback to, plus when the IME was last shown. The timestamp guards a
     * real conflict: if an application's own click handler re-shows the
     * keyboard (e.g. tapping a search-history row re-runs the search), the
     * bubbled dismiss callback would immediately hide it again.
     */
    lv_obj_t *dismiss_root;
    lv_obj_t *dismiss_layer;
    uint32_t  shown_tick;

    ime_mode_t mode;
    ime_lang_t lang;
    lv_pinyin_ime_panel_t panel;
    /*
     * 语音页**自己搭**的控件（见 kb.c 的 voice_page_build）：状态文字 + 圆形麦克风键。
     * 每次 rebuild 都会重建，所以它们非空就等于"当前在语音页"。
     */
    lv_obj_t *voice_page_status;
    lv_obj_t *voice_page_mic;
    bool panel_alt;        /**< P1 is showing the "other symbols" batch */
    bool t9_row_visible;   /**< 选拼音 toggle */

    /** Composition text with one apostrophe per syllable boundary ("la'wan'le"). */
    char pinyin_text[IME_UI_PINYIN_TEXT_MAX];

    /* candidate bar */
    lv_obj_t *cand_bar;
    lv_obj_t *cand_row;
    lv_obj_t *page_prev;
    lv_obj_t *page_next;
    lv_obj_t *chip;
    lv_obj_t *cand_btns[IME_UI_CAND_MAX];
    lv_obj_t *cand_labels[IME_UI_CAND_MAX];

    /* 9-key pinyin row */
    lv_obj_t *t9_row;
    lv_obj_t *t9_btns[IME_UI_T9_PINYIN_MAX];
    lv_obj_t *t9_labels[IME_UI_T9_PINYIN_MAX];

    /* keyboard */
    lv_obj_t *kb_rows;

    /*
     * 最近一次布局求解的结果（ime_layout_measure/solve/validate 的输出）。
     *
     * ★ 留在 ctx 里而不是当场丢掉：真机上"某一行偏了 / 某个键被裁了"只能靠
     *   lv_pinyin_ime_dump() 打出来的这几行数字判断，而重新求解一次需要键盘表和
     *   当前尺寸 —— 那正是出问题时最不可靠的两样东西。开销是每个控件几十字节。
     */
    ime_layout_metrics_t kb_metrics;
    ime_layout_report_t  kb_report;
    /** 当前键盘建出来的键数（= 平铺在 kb_rows 下的子对象个数）。 */
    size_t kb_key_count;
    /** 上一次求解时 kb_rows 的内容宽度：尺寸回调靠它判断"宽度真的变了没有"。 */
    int32_t kb_area_w;

    /*
     * Backspace hold state: LVGL repeats LV_EVENT_LONG_PRESSED_REPEAT every
     * long_press_repeat_time (100 ms by default) while the key is held, and the
     * same hold doubles as the "slide left to clear" gesture. All of it lives
     * here so a rebuild of the key rows cannot lose it.
     */
    lv_obj_t *bksp_hint;      /**< the "← 左滑清空输入框" pill (built lazily) */
    int32_t   bksp_press_x;   /**< finger x when the backspace key went down */
    int32_t   bksp_drag_px;   /**< slide distance that clears the text area */
    bool      bksp_hold;      /**< long press reached: hint shown, gesture armed */
    bool      bksp_cleared;   /**< this hold already cleared the text area once */
    bool      bksp_consumed;  /**< a repeat ran: swallow the synthetic CLICKED */

    /*
     * 语音输入（2026-11）。lv_pinyin_ime_voice_attach() 才会建会话；没接时 voice
     * 为 NULL，键盘上的语音键点了只打一条警告。
     *
     * 线程约定：下面的字段只允许在 LVGL 线程里读写；组件任务那边只发
     * lv_async_call（见 src/voice/ime_voice_ui.c 开头的说明）。
     */
    void *voice;                 /**< lv_pinyin_ime_voice_t*（这里用 void* 免得
                                  *   UI 私有头把语音的类型也拖进来） */
    lv_obj_t *voice_status;      /**< 候选栏里的状态文案（聆听中 / 识别中 / 错误） */
    lv_obj_t *voice_bar;         /**< 控件上沿的时长浮窗（和拼音浮窗同样式） */
    /* voice_page_status 在结构体前面已经声明过了（语音页那一行状态文字）。 */
    /**
     * 已经写进输入框的预览长度。
     *
     * ★ 单位是**字符**，不是字节：LVGL 的 lv_textarea_delete_char() 删的是**一个
     *   UTF-8 字符**（看它的实现：`lv_text_cut(label_txt, pos - 1, 1)`），而一个
     *   汉字是 3 字节。拿字节数去删会多删两倍的字 —— 中文下每次实时刷新都会啃掉
     *   一截已经上屏的文字，表现为"看着像没有实时上屏 / 越刷越短"。
     */
    uint32_t  voice_preview_chars;
    /**
     * 本次预览开始**之前**输入框的内容（快照）。
     *
     * 用来确认"那段预览还在原位"：用户中途手改了输入框、或输入框里本来就有内容时，
     * 快照对不上就只追加、不删 —— 绝不误删用户自己打的内容。
     */
    char      voice_base[IME_VOICE_BASE_MAX];
    /** 上一次写进输入框的文本，用来去重（100 ms 轮询 vs 服务端几百毫秒回一片）。 */
    char      voice_shown[IME_VOICE_PREVIEW_MAX];
    /** 本次会话刷新了几次预览（诊断"看着像没有实时上屏"用）。 */
    uint32_t  voice_preview_updates;
    bool      voice_status_shown;/**< 候选行是否正被语音状态占用 */
    bool      voice_committed;   /**< 本次会话的结果已经上屏 */
    uint32_t  voice_done_tick;   /**< 进入 DONE/ERROR 的时刻，用于停留计时 */
} lv_pinyin_ime_ctx_t;

/** State pointer of a root object, or NULL. */
lv_pinyin_ime_ctx_t *ime_ctx_get(lv_obj_t *obj);

/** lv_event callback that frees the context together with the root object. */
void ime_root_delete_cb(lv_event_t *e);

/* ---------------------------------------------------------------- fit sizing */

/**
 * Re-derive the row height and re-solve the current keyboard's geometry, then
 * move/resize the keys that are already on screen.
 *
 * Two callers, one reason: IME_UI_FIT_HEIGHT_PX changes the row height (and with
 * it the whole keyboard), and the 9-key pinyin row appearing/disappearing changes
 * how many rows share that height. Rebuilding the objects instead would be a
 * waste - and would drop the backspace hold state.
 */
void ime_ui_apply_sizes(lv_pinyin_ime_ctx_t *ctx);

/**
 * Derive IME_UI_ROW_HEIGHT_PX from IME_UI_FIT_HEIGHT_PX and the number of rows
 * @p kb will need (candidate bar + optional 9-key pinyin row + key rows).
 *
 * Called *before* a keyboard is built (see ime_ui_rebuild_keyboard) so the keys
 * are created at the final height instead of being moved right afterwards. No-op
 * when fitting is disabled: the row height is a constant then.
 */
void ime_ui_fit_row_height(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb);

/**
 * Height available to the keyboard rows in px, or 0 when the caller does not know
 * it (no fit target): the layout engine then uses the preferred row height.
 */
int32_t ime_ui_keyboard_area_height(const lv_pinyin_ime_ctx_t *ctx);

/** Re-solve the active keyboard and move the keys already on screen (may rebuild). */
void ime_ui_relayout_keyboard(lv_pinyin_ime_ctx_t *ctx);

/** Hook kb_rows' LV_EVENT_SIZE_CHANGED (called once, from create()). */
void ime_ui_keyboard_watch_size(lv_pinyin_ime_ctx_t *ctx);

/* ------------------------------------------------------------- candidates */

/** Create the candidate bar, the pinyin chip and the 9-key pinyin row. */
void ime_ui_build_candidates(lv_pinyin_ime_ctx_t *ctx);

/**
 * 允许拼音浮窗画到控件外面。
 *
 * 浮窗贴在候选栏上沿之上 —— 那是控件内容区**之外**，而 LVGL 默认把子对象裁在父
 * 对象盒子里。所以创建控件时要调一次：它会给控件本身和每一层祖先都加上
 * "允许溢出 + 撑高扩展绘制区"（少一层就会被那一层裁掉）。
 */
void ime_ui_chip_prepare(lv_obj_t *root);

/** Redraw the candidate list, the chip and the pager state. */
void ime_ui_refresh_candidates(lv_pinyin_ime_ctx_t *ctx);

/** Redraw the 9-key pinyin row. */
void ime_ui_refresh_t9(lv_pinyin_ime_ctx_t *ctx);

/* --------------------------------------------------------------- keyboard */

/** Rebuild the key rows for the current mode / panel / language. */
void ime_ui_rebuild_keyboard(lv_pinyin_ime_ctx_t *ctx);

/** Keyboard description for the active mode / panel / language. */
const ime_keyboard_t *ime_ui_active_keyboard(const lv_pinyin_ime_ctx_t *ctx);

/**
 * Solve the geometry of @p kb for the current keyboard area and fill @p rects
 * (row-major key order, ime_layout_key_count(kb) entries).
 *
 * Fills ctx->kb_metrics / ctx->kb_report with what was measured and checked, so
 * lv_pinyin_ime_dump() can print the numbers the layout was built from.
 */
bool ime_ui_calculate_layout(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb,
                             ime_key_rect_t *rects, size_t cap);

/**
 * Create the buttons of one keyboard (cleaning whatever was there before).
 *
 * Geometry comes from ime_ui_calculate_layout(); this function only creates
 * objects, applies styles and wires the existing event callbacks, so layout and
 * interaction can be changed independently.
 */
void ime_ui_apply_keyboard(lv_pinyin_ime_ctx_t *ctx, const ime_keyboard_t *kb);

/* ------------------------------------------------------------------- misc */

/**
 * Move every pending commit from the session into the bound text area.
 *
 * The session buffers committed text and the UI drains it in one place. Any
 * interaction that can commit (a key press *or* a candidate tap) has to call
 * this, or the text sits in the buffer until some later key press flushes it -
 * which looks like "the word only appears after I press another key".
 */
void ime_ui_commit_pending(lv_pinyin_ime_ctx_t *ctx);

/**
 * Switch the input language and drop the shift state with it.
 *
 * Chinese draws its letters upper case and ignores the caps-lock key, so a lock
 * kept across a language switch could not be cleared while Chinese is active and
 * would silently come back with English. Every language change goes through
 * here so the keyboard is rebuilt exactly once, in a consistent state.
 */
void ime_ui_set_lang(lv_pinyin_ime_ctx_t *ctx, ime_lang_t lang);

/** Refresh everything the session can change. */
void ime_ui_refresh_all(lv_pinyin_ime_ctx_t *ctx);

/** Dispatch one key press. */
void ime_ui_handle_key(lv_pinyin_ime_ctx_t *ctx, const ime_key_t *key);

/* ---------------------------------------------------------------- 语音输入 */

#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE
/*
 * 语音与控件之间的那几个入口在 src/voice/ime_voice_ui.h（单独的粘合头）。
 *
 * ★ 为什么不在这里重复声明一遍：两个头对同一个函数写出不同形参类型
 *   （那边为了不依赖本模块的类型写成 void*）会构成冲突声明，在 -Werror 下直接
 *   编译失败。让唯一的声明留在粘合头里，两边都 include 它。
 */
#include "voice/ime_voice_ui.h"
#endif

/* ------------------------------------------------------- backspace behaviour */

/**
 * Delete one character: the pinyin buffer first, then the bound text area.
 *
 * Shared by the click handler and the hold-to-repeat handler - the repeat used
 * to call ime_session_backspace() on its own, which does nothing at all once the
 * composition buffer is empty, so holding the key only ever deleted the single
 * character the release-time CLICKED removed.
 */
void ime_ui_backspace(lv_pinyin_ime_ctx_t *ctx);

/** Backspace key went down at @p finger_x: start a fresh hold. */
void ime_ui_bksp_hold_begin(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x);

/** Long press reached: show the hint and arm the slide-to-clear gesture. */
void ime_ui_bksp_hold_arm(lv_pinyin_ime_ctx_t *ctx);

/** Pointer moved to @p finger_x while held: clear once the slide is long enough. */
void ime_ui_bksp_hold_move(lv_pinyin_ime_ctx_t *ctx, int32_t finger_x);

/** The hold ended (release or press lost): hide the hint and disarm. */
void ime_ui_bksp_hold_end(lv_pinyin_ime_ctx_t *ctx);

/**
 * True when the pending CLICKED belongs to a hold that already deleted
 * something. LVGL sends CLICKED unconditionally after a long press, so without
 * this the release would delete one character more than the user asked for.
 * Reading it clears the flag; the flag is also cleared on the next press.
 */
bool ime_ui_bksp_take_consumed(lv_pinyin_ime_ctx_t *ctx);

/** Map a T9 key caption ("ABC") to its digit; '0' when unknown. */
char ime_ui_digit_for_group(const char *caption);

/** Full width form of a punctuation label in Chinese mode. */
const char *ime_ui_fullwidth(const lv_pinyin_ime_ctx_t *ctx, const char *label);

/** One shot / lock / off cycle for the shift key. */
void ime_ui_cycle_shift(lv_pinyin_ime_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
