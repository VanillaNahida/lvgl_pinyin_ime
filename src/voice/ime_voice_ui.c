/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音输入与键盘控件的粘合层：语音键、状态条、输入框预览与最终上屏。
 *
 * 线程边界（这是本文件存在的唯一理由）：
 *   会话的状态/结果回调跑在**组件任务**里，那里碰 LVGL 就是踩雷。LVGL 9 的
 *   lv_async_call() 不会排队重复的回调（同一函数+同一 user_data 只留一个），
 *   所以可以安全地当"有个更新待处理"的标志来用：
 *     组件任务      → 置 ctx->voice->... 标志 + lv_async_call（唤醒 LVGL 线程）
 *     LVGL 线程     → ime_ui_voice_sync() 里读会话状态、改控件
 *
 * 状态条的视觉：候选栏之上有一条和"拼音浮窗"同样式的浮窗（右上角，显示聆听时长），
 * 候选栏本身在语音会话期间换成状态文案（"聆听中" / "识别中" / 错误原因）。状态条
 * 只依赖已经跑通的 FLOATING + 溢出绘制路径，不引入新的布局约束。
 */

#include "voice/ime_voice_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ime_log.h"
#include "data/ime_font.h"
#include "lv_pinyin_ime_internal.h"

static const char *TAG = "ime_voice_ui";

/* ------------------------------------------------------------------ 小工具 */

static lv_pinyin_ime_ctx_t *ctx_of(lv_obj_t *ime)
{
    return (ime != NULL) ? ime_ctx_get(ime) : NULL;
}

/** 会话是否处于"正在进行"的状态。 */
static bool state_active(lv_pinyin_ime_voice_state_t st)
{
    return st == LV_PINYIN_IME_VOICE_CONNECTING || st == LV_PINYIN_IME_VOICE_LISTENING ||
           st == LV_PINYIN_IME_VOICE_FINISHING;
}

/**
 * 字体里有没有这个字形。
 *
 * ★ 不能只看 lv_font_get_glyph_dsc() 的返回值：字体缺字时它**返回 true**，只是把
 *   is_placeholder 置起来（缺字画成方框）。所以要检查那个标志。候选栏的翻页箭头
 *   （lv_pinyin_ime_cand.c 的 pager_caption）用的是同一套判据。
 */
static bool font_has_cp(const lv_font_t *font, uint32_t cp)
{
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, cp, '\0') && !dsc.is_placeholder;
}

/**
 * 数一个 UTF-8 串里有几个字符（LVGL 的删除函数是按字符算的，见下）。
 *
 * 只数首字节：`0xxxxxxx`、`110xxxxx`、`1110xxxx`、`11110xxx` 都不会是 `10xxxxxx`
 * 的续字节，所以"不是续字节的字节数"就是字符数。
 */
static uint32_t utf8_count_chars(const char *s, size_t len)
{
    uint32_t n = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (((unsigned char)s[i] & 0xC0u) != 0x80u) {
            n++;
        }
    }
    return n;
}

/**
 * 把识别结果写进输入框。
 *
 * ★ 为什么不能用"字节数"记账（这里踩过坑）：
 *
 *   LVGL 的 `lv_textarea_delete_char()` 删的是**一个 UTF-8 字符**，而不是一个字节
 *   （看它的实现：`lv_text_cut(label_txt, pos - 1, 1)`）。而一个汉字是 3 字节，
 *   所以拿"上次写了多少字节"当成"要删几个字符"会**多删两倍的字** —— 中文场景下
 *   每次实时刷新都会把已经上屏的文字啃掉一大块，表现为"越刷越少/看着像没上屏"。
 *
 *   ⚠ 我自己那个 C 测试覆盖不到这条路径：`host_voice_test` 测的是协议层（段落表），
 *     而这里是 LVGL 控件行为，只有真机或 LVGL 主机端渲染测试才碰得到。
 *
 * ★ 记一次快照（ctx->voice_base）：如果用户中途手改了输入框，快照对不上就**不删**、
 *   直接追加 —— 宁可多几个字，也不要把用户自己打的、或上一句话的内容删掉。
 *
 * @param text          要写入的文本
 * @param char_count    写入的字符数（不是字节数！）
 * @param base_snapshot 本段预览开始**之前**的输入框内容（用于校验是否被改过）
 */
static void voice_write_text(lv_obj_t *ta, const char *text, uint32_t char_count,
                             const char *base_snapshot)
{
    if (ta == NULL || text == NULL) {
        return;
    }

    /* 1. 先试着撤掉上一次预览：只有在"输入框仍以那段基线开头"时才敢删。 */
    if (char_count > 0 && base_snapshot != NULL) {
        const char *cur = lv_textarea_get_text(ta);
        const size_t base_len = strlen(base_snapshot);
        if (cur != NULL && base_len > 0 && strncmp(cur, base_snapshot, base_len) == 0 &&
            cur[base_len] != '\0') {
            /* 预览就接在基线后面：从 base_len 处开始删 char_count 个字符。
             * 删一个字符后后面的文字会前移，所以每次都从同一位置删。 */
            for (uint32_t i = 0; i < char_count; i++) {
                if (utf8_count_chars(cur, strlen(cur)) <= utf8_count_chars(base_snapshot,
                                                                          base_len)) {
                    break;      /* 已经删到只剩基线了 */
                }
                lv_textarea_set_cursor_pos(ta, (uint32_t)base_len);
                lv_textarea_delete_char(ta);
                cur = lv_textarea_get_text(ta);
            }
        }
    }

    /* 2. 追加新文本 */
    lv_textarea_add_text(ta, text);
}

/* ------------------------------------------------------------------ 异步唤醒 */

/**
 * 组件任务里调用：请求 LVGL 线程做一次同步。
 *
 * 重复调用是安全的：lv_async_call 对同一 (函数, user_data) 只会保留一个待执行的
 * 回调，所以状态连续变化不会堆积。
 */
static void voice_task_notify(lv_pinyin_ime_voice_t *v, void *user)
{
    (void)v;
    lv_obj_t *ime = (lv_obj_t *)user;
    if (ime != NULL && lv_obj_is_valid(ime)) {
        (void)lv_async_call(ime_ui_voice_sync_async, ime);
    }
}

/* ---------------------------------------------------------------- 状态条 */

/* --------------------------------------------------- 语音事件码（公开 API） */

/*
 * 两个自定义事件码：状态变化 / 最终结果已上屏（声明见
 * include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h）。
 *
 * 用和核心那两个事件码（lv_pinyin_ime_event_ready / _cand_changed，见
 * src/ui/lv_pinyin_ime.c）完全相同的惰性注册写法：第一次问的时候才登记，
 * 保证"没用事件的应用不占号"，也保证两个函数拿到的是同一对号。
 *
 * ★ 之前这两个只有声明、没有定义，而 voice_poll_cb() 已经在用它们 ——
 *   默认关闭语音时 src/voice/ 下的文件不参与编译，所以直到
 *   CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y 才在链接期暴露出来。
 */
static bool            s_voice_events_registered;
static lv_event_code_t s_evt_voice_state;
static lv_event_code_t s_evt_voice_result;

lv_event_code_t lv_pinyin_ime_event_voice_state(void)
{
    if (!s_voice_events_registered) {
        s_evt_voice_state = lv_event_register_id();
        s_evt_voice_result = lv_event_register_id();
        s_voice_events_registered = true;
    }
    return s_evt_voice_state;
}

lv_event_code_t lv_pinyin_ime_event_voice_result(void)
{
    if (!s_voice_events_registered) {
        (void)lv_pinyin_ime_event_voice_state();
    }
    return s_evt_voice_result;
}

static void voice_bar_ensure(lv_pinyin_ime_ctx_t *ctx)
{
    if (ctx->voice_bar != NULL) {
        return;
    }
    ctx->voice_bar = lv_label_create(ctx->obj);
    lv_label_set_text(ctx->voice_bar, "");
    ime_style_apply_chip(ctx->voice_bar);
    ime_style_apply_font(ctx->voice_bar, ime_font_small());
    lv_obj_remove_flag(ctx->voice_bar, LV_OBJ_FLAG_CLICKABLE);
    /*
     * FLOATING + HIDDEN，和拼音浮窗（lv_pinyin_ime_cand.c 的 chip）完全一致：
     * 既不参与 flex 布局、也不算进父对象的内容高度。改成 IGNORE_LAYOUT 会让
     * "父对象多高取决于子对象、子对象位置又取决于父对象"变成自反馈，LVGL 会一直
     * 重排（见该文件里的看门狗注释）。
     */
    lv_obj_add_flag(ctx->voice_bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(ctx->voice_bar, LV_OBJ_FLAG_HIDDEN);

    /* 右上角、控件内容区之外：和拼音浮窗分居两侧，互不重叠。 */
    const lv_font_t *font = ime_font_small();
    const int line_h = (font != NULL) ? (int)font->line_height : 20;
    lv_obj_align(ctx->voice_bar, LV_ALIGN_TOP_RIGHT, -(IME_UI_PAD_X + 2), -(line_h + 5));
}

/** 更新状态条（会话进行中才显示）。 */
static void voice_bar_update(lv_pinyin_ime_ctx_t *ctx)
{
    lv_pinyin_ime_voice_t *v = ctx->voice;
    const lv_pinyin_ime_voice_state_t st =
        (v != NULL) ? lv_pinyin_ime_voice_state(v) : LV_PINYIN_IME_VOICE_IDLE;

    if (!state_active(st)) {
        if (ctx->voice_bar != NULL) {
            lv_obj_add_flag(ctx->voice_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    voice_bar_ensure(ctx);
    if (ctx->voice_bar == NULL) {
        return;
    }

    /* 内容：时长（"+1.2s"）。用 .1f 之外的形式拼，避免在任务栈上拉浮点格式化。 */
    const uint32_t ms = lv_pinyin_ime_voice_audio_ms(v);
    char buf[24];
    snprintf(buf, sizeof(buf), "%u.%us", (unsigned)(ms / 1000u), (unsigned)((ms / 100u) % 10u));
    lv_label_set_text(ctx->voice_bar, buf);
    lv_obj_remove_flag(ctx->voice_bar, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ 候选栏状态文案 */

/** 会话进行中/刚结束时，用候选栏的位置显示状态；空闲时交还给候选词。 */
static void voice_status_update(lv_pinyin_ime_ctx_t *ctx)
{
    lv_pinyin_ime_voice_t *v = ctx->voice;
    const lv_pinyin_ime_voice_state_t st =
        (v != NULL) ? lv_pinyin_ime_voice_state(v) : LV_PINYIN_IME_VOICE_IDLE;

    /*
     * 只在"正在进行"或"刚出结果/出错"时占用候选栏。DONE / ERROR 停留一会儿再交还
     * （见末尾的 sync），这样用户能看到"完成"或错误原因，而不是一闪而过。
     */
    const bool show = state_active(st) || st == LV_PINYIN_IME_VOICE_DONE ||
                      st == LV_PINYIN_IME_VOICE_ERROR;
    if (show == ctx->voice_status_shown) {
        return;
    }
    ctx->voice_status_shown = show;

    if (show) {
        lv_obj_add_flag(ctx->cand_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ctx->cand_row, LV_OBJ_FLAG_HIDDEN);
        if (ctx->voice_status != NULL) {
            lv_obj_add_flag(ctx->voice_status, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** 画状态文案（放在候选行原来的位置）。 */
static void voice_status_label(lv_pinyin_ime_ctx_t *ctx)
{
    lv_pinyin_ime_voice_t *v = ctx->voice;
    if (!ctx->voice_status_shown || ctx->voice_status == NULL || v == NULL) {
        return;
    }

    const char *text = lv_pinyin_ime_voice_state_text(v);
    if (text == NULL || text[0] == '\0') {
        /* 没有任何文案时别留一条空栏：让候选行回来。 */
        lv_obj_add_flag(ctx->voice_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(v);

    /*
     * 错误文案优先用"图标 + 文字"，图标取自 LVGL 自带的符号字体（在 FontAwesome
     * 私用区，子集化的中文字体里没有）。所以先探字形，没有就退回纯文字。
     */
    const char *icon = NULL;
    if (st == LV_PINYIN_IME_VOICE_ERROR) {
        icon = LV_SYMBOL_WARNING;
    } else if (st == LV_PINYIN_IME_VOICE_DONE) {
        icon = LV_SYMBOL_OK;
    }
    const bool icon_ok = (icon != NULL) && font_has_cp(LV_FONT_DEFAULT, (uint32_t)icon[0]);

    if (st == LV_PINYIN_IME_VOICE_LISTENING) {
        /* 聆听中最需要"它在听"的反馈：用麦克风图标 + 文案。 */
        const char *mic = LV_SYMBOL_AUDIO;
        if (font_has_cp(LV_FONT_DEFAULT, (uint32_t)mic[0])) {
            lv_label_set_text_fmt(ctx->voice_status, "%s %s", mic, text);
        } else {
            lv_label_set_text(ctx->voice_status, text);
        }
    } else if (icon_ok) {
        lv_label_set_text_fmt(ctx->voice_status, "%s %s", icon, text);
    } else {
        lv_label_set_text(ctx->voice_status, text);
    }

    ime_style_apply_font(ctx->voice_status, ime_font_small());
    lv_obj_remove_flag(ctx->voice_status, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------- 提交 */

/** 把会话的最终文本送进输入框。 */
static void voice_commit(lv_pinyin_ime_ctx_t *ctx)
{
    lv_pinyin_ime_voice_t *v = ctx->voice;
    if (v == NULL || ctx->ta == NULL) {
        return;
    }
    const char *text = lv_pinyin_ime_voice_text(v);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    /*
     * 最终文本 = 撤掉本次预览 + 写入最终结果。
     *
     * 不论"追加"还是"替换"模式，这里都是同一个动作：把这段预览换成最终文本。
     * 两者的区别只在于**预览期间**怎么显示（见 ime_ui_voice_sync），到落盘这一刻
     * 没有区别 —— 早期版本在这里给两种模式写了两套分支，反而绕。
     */
    voice_write_text(ctx->ta, text, ctx->voice_preview_chars, ctx->voice_base);

    /* 落盘之后这段不再是预览：清掉记账，下一次会话会重新拍快照。 */
    ctx->voice_preview_chars = 0;
    ctx->voice_base[0] = '\0';

    lv_obj_send_event(ctx->obj, lv_pinyin_ime_event_voice_result(), NULL);
}

/* ---------------------------------------------------------------- 定时器 */

/**
 * 轮询会话状态。
 *
 * 为什么用轮询而不是纯回调：回调在组件任务里，只能给 LVGL 线程发"有事发生"的信号，
 * 真正的状态值还是要有人读。这里 100 ms 读一次，顺带处理 DONE/ERROR 的停留与复位。
 */
static void voice_poll_cb(lv_timer_t *t)
{
    lv_obj_t *ime = (lv_obj_t *)lv_timer_get_user_data(t);
    if (ime == NULL || !lv_obj_is_valid(ime)) {
        return;
    }
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL || ctx->voice == NULL) {
        lv_timer_delete(t);
        return;
    }

    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(ctx->voice);

    /* 结果落地：DONE 时提交文本；ERROR 时也提交（已经识别的部分别丢）。 */
    if ((st == LV_PINYIN_IME_VOICE_DONE || st == LV_PINYIN_IME_VOICE_ERROR) &&
        !ctx->voice_committed && lv_pinyin_ime_voice_result_ready(ctx->voice)) {
        ctx->voice_committed = true;
        voice_commit(ctx);
    }

    voice_status_label(ctx);
    voice_bar_update(ctx);

    /* 停留计时：让"完成 / 错误原因"能被看见，然后交还候选栏。 */
    if (st == LV_PINYIN_IME_VOICE_DONE || st == LV_PINYIN_IME_VOICE_ERROR) {
        if (ctx->voice_done_tick == 0) {
            ctx->voice_done_tick = lv_tick_get();
        } else if (lv_tick_elaps(ctx->voice_done_tick) > IME_VOICE_DONE_LINGER_MS) {
            ctx->voice_status_shown = false;
            lv_obj_remove_flag(ctx->cand_row, LV_OBJ_FLAG_HIDDEN);
            if (ctx->voice_status != NULL) {
                lv_obj_add_flag(ctx->voice_status, LV_OBJ_FLAG_HIDDEN);
            }
            if (ctx->voice_bar != NULL) {
                lv_obj_add_flag(ctx->voice_bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        ctx->voice_done_tick = 0;
    }
}

/* ---------------------------------------------------------------- 公开 */

/* 由 lv_pinyin_ime.c 的删除回调调用：清掉属于本会话的 LVGL 资源与回调。 */
void ime_ui_voice_detach(void *ctx_arg)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)ctx_arg;
    if (ctx == NULL) {
        return;
    }
    /*
     * 取消可能还挂着的 async。这个调用**不依赖任何标志位**：lv_async_call_cancel()
     * 在"没有对应的待执行项"时只是返回一个失败码，代价可忽略；而如果漏掉它，
     * 组件任务刚排进来的那个回调就会在控件销毁后执行，拿的是已经释放的 ctx。
     *
     * （早先这里用一个 voice_sync_pending 字段做门控，但那个字段从来没有人置位 ——
     *   死代码，而且会给人"已经处理过了"的错觉。）
     */
    (void)lv_async_call_cancel(ime_ui_voice_sync_async, ctx->obj);
    if (ctx->voice != NULL) {
        /* 先销毁会话：它会停任务，之后就不会再有回调进来。 */
        lv_pinyin_ime_voice_destroy(ctx->voice);
        ctx->voice = NULL;
    }
    ctx->voice_status = NULL;
    ctx->voice_bar = NULL;
    ctx->voice_page_status = NULL;
    ctx->voice_preview_chars = 0;
    ctx->voice_base[0] = '\0';
    ctx->voice_shown[0] = '\0';
}

void ime_ui_voice_sync(void *ctx_arg)
{
    lv_pinyin_ime_ctx_t *ctx = (lv_pinyin_ime_ctx_t *)ctx_arg;
    if (ctx == NULL || ctx->voice == NULL) {
        return;
    }
    voice_status_update(ctx);
    voice_status_label(ctx);
    voice_bar_update(ctx);

    /*
     * 语音页自带一行状态文字（那一页里候选栏是隐藏的，状态就靠它显示）。
     * 控件由 kb.c 的 voice_page_build() 建，非空即"当前在语音页"。
     */
    if (ctx->voice_page_status != NULL) {
        lv_label_set_text(ctx->voice_page_status, lv_pinyin_ime_voice_state_text(ctx->voice));
    }

    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(ctx->voice);

    /*
     * 实时预览：把当前识别到的文本写进输入框。
     *
     * ★ 这里**不看 status**，只要还在识别就写 —— 服务端开 wpgs 后每一片结果
     *   （status=0/1）都带着当前整句，收到就该显示，这才有"边说边出字"。
     *   只在 FINISHING 之后停止刷新（收尾由 voice_commit 一次性落盘）。
     *
     * 只在文本真的变了时才动控件：轮询是 100 ms 一次，而服务端几百毫秒才回一片，
     * 不去重的话每秒会重写 10 次输入框（光标跳动、白重画）。
     */
    const lv_pinyin_ime_voice_cfg_t *cfg = lv_pinyin_ime_voice_get_config(ctx->voice);
    const bool live = (cfg == NULL) || cfg->live_preview;
    const bool busy = (st == LV_PINYIN_IME_VOICE_CONNECTING) ||
                      (st == LV_PINYIN_IME_VOICE_LISTENING);
    if (live && busy && ctx->ta != NULL) {
        const char *text = lv_pinyin_ime_voice_text(ctx->voice);
        if (text != NULL && text[0] != '\0' && strcmp(text, ctx->voice_shown) != 0) {
            /* 第一次写之前先拍快照：记下"预览之前"输入框长什么样，
             * 之后每次刷新都靠它确认自己没有误删用户的内容。 */
            if (ctx->voice_base[0] == '\0') {
                const char *cur = lv_textarea_get_text(ctx->ta);
                snprintf(ctx->voice_base, sizeof(ctx->voice_base), "%s",
                         (cur != NULL) ? cur : "");
            }
            voice_write_text(ctx->ta, text, ctx->voice_preview_chars, ctx->voice_base);
            ctx->voice_preview_chars = utf8_count_chars(text, strlen(text));
            snprintf(ctx->voice_shown, sizeof(ctx->voice_shown), "%s", text);
            ctx->voice_preview_updates++;
        }
    }
}

void ime_ui_voice_sync_async(void *user_data)
{
    lv_obj_t *ime = (lv_obj_t *)user_data;
    if (ime == NULL || !lv_obj_is_valid(ime)) {
        return;
    }
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL) {
        return;
    }
    ime_ui_voice_sync(ctx);
}

lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_attach(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL) {
        return NULL;
    }
    if (ctx->voice != NULL) {
        return ctx->voice;              /* 已经接过，直接返回同一个 */
    }
    if (!lv_pinyin_ime_voice_supported()) {
        IME_LOGW(TAG, "voice input is not compiled in");
        return NULL;
    }

    lv_pinyin_ime_voice_cfg_t cfg = lv_pinyin_ime_voice_cfg_default();
    /*
     * 端点/语种跟着键盘的语言走：英文键盘配 en_us，中文键盘配 zh_cn。调用方之后
     * 可以用 set_config() 覆盖（比如从 NVS 里取密钥）。
     */
    if (ctx->lang == IME_LANG_EN) {
        snprintf(cfg.language, sizeof(cfg.language), "%s", "en_us");
    }

    lv_pinyin_ime_voice_t *v = lv_pinyin_ime_voice_create(&cfg);
    if (v == NULL) {
        return NULL;
    }

    ctx->voice = v;
    lv_pinyin_ime_voice_set_state_cb(v, voice_task_notify, ime);
    lv_pinyin_ime_voice_set_result_cb(v, voice_task_notify, ime);

    /* 状态条 */
    lv_obj_t *label = lv_label_create(ctx->cand_bar);
    ime_style_apply_font(label, ime_font_small());
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    ctx->voice_status = label;

    /* 轮询定时器：100 ms 看一次会话状态（见 voice_poll_cb 的说明）。 */
    lv_timer_create(voice_poll_cb, 100, ime);

    IME_LOGI(TAG, "voice input attached (language=%s endpoint=%s)",
             cfg.language, cfg.endpoint);
    return v;
}

lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_of(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    return (ctx != NULL) ? ctx->voice : NULL;
}

/*
 * 组件内部入口：由 lv_pinyin_ime_create() 在**键盘建好之后**调用（见 lv_pinyin_ime.c
 * 里那句注释"建会话并挂到控件上"）。
 *
 * 实现上就是复用上面那个公开函数 —— 两者要做的事完全一样（建会话、挂状态回调、
 * 建候选栏状态 label、起 100 ms 轮询），而公开版已经带了幂等保护
 * （`if (ctx->voice != NULL) return ctx->voice;`），所以 create() 里调一次、
 * 应用之后再调 lv_pinyin_ime_voice_attach() 也不会建出第二个会话。
 *
 * ★ 为什么这里不直接删掉、让应用只调公开版：create() 里这一句保证"键盘一建出来，
 *   语音的 UI 骨架（状态条、轮询）就已经就位"，应用只需要补 host（
 *   lv_pinyin_ime_voice_set_config()）与密钥，接入顺序不受限 —— 这正是下面那句
 *   注释承诺的行为（"先接键盘、后接网络"）。
 *
 * 之前这个函数只有声明和调用、没有定义，于是 CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y
 * 一打开就在链接期报 `undefined reference to ime_ui_voice_attach`
 * （默认关闭时 src/voice/ 下的文件根本不参与编译，所以一直没暴露）。
 */
void *ime_ui_voice_attach(lv_obj_t *ime)
{
    return lv_pinyin_ime_voice_attach(ime);
}

void lv_pinyin_ime_voice_key_pressed(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL || ctx->voice == NULL) {
        return;
    }
    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(ctx->voice);

    /*
     * 已经在识别：TOGGLE / BOTH 下，这一次按下就是"结束"（松手不会再做别的）。
     * PTT 下什么都不做 —— 用户还按着，结束要等松手。
     */
    if (state_active(st)) {
        if (lv_pinyin_ime_voice_tap_mode() != LV_PINYIN_IME_VOICE_TAP_PTT) {
            lv_pinyin_ime_voice_stop(ctx->voice);
        }
        return;
    }

    /* 空闲：开始一次新会话，先复位上一轮的记账。 */
    ctx->voice_preview_chars = 0;
    ctx->voice_base[0] = '\0';          /* 快照在第一次预览时重新拍 */
    ctx->voice_shown[0] = '\0';         /* 去重用的"上次写的文本" */
    ctx->voice_preview_updates = 0;
    ctx->voice_committed = false;
    ctx->voice_done_tick = 0;
    ctx->voice_status_shown = false;    /* 让 voice_status_update 重新接管候选栏 */

    /*
     * 语种跟着键盘的语言走：中文键盘用 zh_cn，英文键盘用 en_us。
     * 每次开始都设一次（而不是只在 attach 时设），这样用户切了中/英之后立刻生效。
     * 注意只动 language 一项，其它字段（尤其是密钥）保持应用设过的值。
     */
    lv_pinyin_ime_voice_cfg_t cfg = *lv_pinyin_ime_voice_get_config(ctx->voice);
    snprintf(cfg.language, sizeof(cfg.language), "%s",
             (ctx->lang == IME_LANG_EN) ? "en_us" : "zh_cn");
    lv_pinyin_ime_voice_set_config(ctx->voice, &cfg);

    const int rc = lv_pinyin_ime_voice_start(ctx->voice);
    if (rc != 0) {
        /* 失败信息已经在状态条上（start 会把状态置 ERROR）。立刻同步一次，
         * 不必等 100 ms 的轮询。 */
        ime_ui_voice_sync(ctx);
    }
}

void lv_pinyin_ime_voice_key_released(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL || ctx->voice == NULL) {
        return;
    }
    /*
     * 松手只在两种情况下做事：
     *   PTT          —— 松手即"我说完了"，结束识别
     *   BOTH + 长按过 —— 长按已经把会话结束了，这里只是兜底（stop 幂等）
     *
     * TOGGLE 模式下松手什么都不做，识别继续跑（用户再按一次才结束）。
     */
    const lv_pinyin_ime_voice_state_t st = lv_pinyin_ime_voice_state(ctx->voice);
    if (st != LV_PINYIN_IME_VOICE_LISTENING) {
        return;
    }
    if (lv_pinyin_ime_voice_tap_mode() == LV_PINYIN_IME_VOICE_TAP_PTT) {
        lv_pinyin_ime_voice_stop(ctx->voice);
    }
}

void lv_pinyin_ime_voice_key_cancelled(lv_obj_t *ime)
{
    lv_pinyin_ime_ctx_t *ctx = ctx_of(ime);
    if (ctx == NULL || ctx->voice == NULL) {
        return;
    }
    lv_pinyin_ime_voice_cancel(ctx->voice);
}
