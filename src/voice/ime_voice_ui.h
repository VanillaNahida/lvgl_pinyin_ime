/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音与键盘控件之间的粘合层接口。
 *
 * 为什么单独一个头：src/ui/ 与 src/voice/ 互相要用对方的东西 ——
 *   ui/lv_pinyin_ime.c  需要在销毁回调里调 ime_ui_voice_detach()、在建控件时调
 *                       ime_ui_voice_attach()
 *   voice/ime_voice_ui.c 需要 ime_ctx_get()、ctx 的字段、以及候选栏/样式的内部函数
 * 而 ui/lv_pinyin_ime_internal.h 又不能 include 语音的公开头（那个头 include 了
 * lvgl.h 并反过来用到 ui 的类型，互相包含绕不出去）。所以：
 *   - 语音公开头（include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h）里放**面向应用**的
 *     类型与函数，它不知道 ui 的内部结构；
 *   - ime_voice_internal.h 放语音模块自己的私有类型（会话结构体、队列长度等）；
 *   - 本文件放**跨模块调用**的那几个函数，签名只用两边都认识的类型（lv_obj_t*、
 *     ctx 指针<void*>）。
 *
 * 全部函数都在 LVGL 线程里调用，唯一的例外见 ime_ui_voice_sync_async。
 */

#pragma once

#include "lvgl.h"

#include "voice/ime_voice_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 建会话并接到控件上；返回会话句柄，不支持 / 失败返回 NULL。
 *
 * 由 lv_pinyin_ime_create() 调用。此时还没有 host 回调（应用要自己
 * lv_pinyin_ime_voice_set_config()），所以这里只做"能建就建"。
 */
void *ime_ui_voice_attach(lv_obj_t *ime);

/** 取控件上挂着的会话；没接返回 NULL。 */
void *ime_ui_voice_of(lv_obj_t *ime);

/** 语音键：按下 / 松开 / 取消。 */
void ime_ui_voice_key_pressed(lv_obj_t *ime);
void ime_ui_voice_key_released(lv_obj_t *ime);
void ime_ui_voice_key_cancelled(lv_obj_t *ime);

/**
 * 把会话状态同步到界面（只能在 LVGL 线程调用）。
 *
 * ★ 形参是 void * 而不是 lv_pinyin_ime_ctx_t *：本头文件要同时被 src/ui/ 和
 *   src/voice/ 包含，而 ctx 的类型定义在 ui/lv_pinyin_ime_internal.h 里（那个头
 *   又 include 本头文件做交叉声明）。C 不允许同一函数出现两个不同的声明，所以
 *   跨模块的这一份统一用 void*；带类型的声明留在 ui/lv_pinyin_ime_internal.h，
 *   只在 src/ui/ 内部使用。
 */
void ime_ui_voice_sync(void *ctx);

/**
 * lv_async_call 的入口：组件任务侧唤醒 LVGL 线程用。
 *
 * ★ 这是唯一一个**可能**在非 LVGL 线程被排入的入口，但它本身由 lv_timer_handler
 *   在 LVGL 线程里执行，所以函数体内碰控件是安全的。
 */
void ime_ui_voice_sync_async(void *user_data);

/** 控件销毁前摘掉语音：停任务、取消挂起的 async。 */
void ime_ui_voice_detach(void *ctx);

#ifdef __cplusplus
}
#endif
