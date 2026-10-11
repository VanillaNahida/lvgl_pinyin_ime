/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音模块内部共享的东西。不进 include/，调用方看不到。
 */

#pragma once

#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

#include "voice/ime_voice_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 待处理返回帧的队列长度。
 *
 * 生产者是调用方的网络任务，消费者是组件任务。正常节奏下服务端每几十毫秒回一帧，
 * 而组件任务每 40 ms 就会清空一次队列，16 条足够吸收抖动；真满了会丢最旧的
 * （见 queue_push），因为新帧更接近当前状态。
 */
#define IME_VOICE_QUEUE_LEN 16

/**
 * 语音键的短按语义（Kconfig: LV_PINYIN_IME_VOICE_TAP_MODE）。
 *
 * 长按对讲是另一条路：按住不放 -> 说话 -> 松手上屏。两种都要时 BOTH，由按键时长
 * 决定走哪条（见 lv_pinyin_ime_kb.c 的语音键回调）。
 */
typedef enum {
    LV_PINYIN_IME_VOICE_TAP_TOGGLE = 0, /**< 短按开始 / 再按结束 */
    LV_PINYIN_IME_VOICE_TAP_PTT,        /**< 按住说话 / 松手结束 */
    LV_PINYIN_IME_VOICE_TAP_BOTH,       /**< 短按切换 + 长按对讲 */
} lv_pinyin_ime_voice_tap_mode_t;

/** 编译期定下的短按语义。 */
lv_pinyin_ime_voice_tap_mode_t lv_pinyin_ime_voice_tap_mode(void);

/**
 * "完成 / 出错"的文案在候选栏停留多久（毫秒）。
 *
 * 不留一会儿的话，用户只看到键盘闪一下，既不知道成了没有，也看不到错误原因
 * （"系统时间未同步"这种提示一闪而过等于没说）。
 */
#define IME_VOICE_DONE_LINGER_MS 1500

#ifdef __cplusplus
}
#endif
