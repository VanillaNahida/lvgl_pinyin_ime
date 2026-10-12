/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音会话的**空实现**，只给主机端渲染测试用（-DIME_HOST_VOICE=ON）。
 *
 * 为什么需要它：语音页的几何（三区段 + 圆形麦克风 + 底部按钮）在
 * src/ui/lv_pinyin_ime_kb.c 的 `#if CONFIG_LV_PINYIN_IME_VOICE_ENABLE` 里，而真的
 * 语音实现（src/voice/ime_voice*.c）要 WebSocket、FreeRTOS、I2S —— 主机上跑不起来。
 * 但"语音页排版对不对"和"会话能不能连上"是两件互不相干的事：这里把会话那一层换成
 * 空实现，"能建控件就能验几何"。
 *
 * ★ 空实现返回 NULL（= 没有会话），所以按键回调里的 lv_pinyin_ime_voice_of() 检查
 *   会走"未接语音"那条分支 —— 不会有人误以为这里验证了识别流程。
 */

#include "voice/ime_voice_ui.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime_voice.h"

/* 粘合层（src/voice/ime_voice_ui.c）的那几个入口 */
void *ime_ui_voice_attach(lv_obj_t *ime)
{
    (void)ime;
    return NULL;   /* no session: the voice key reports "not configured" */
}

void *ime_ui_voice_of(lv_obj_t *ime)
{
    (void)ime;
    return NULL;
}

void ime_ui_voice_key_pressed(lv_obj_t *ime)
{
    (void)ime;
}

void ime_ui_voice_key_released(lv_obj_t *ime)
{
    (void)ime;
}

void ime_ui_voice_key_cancelled(lv_obj_t *ime)
{
    (void)ime;
}

void ime_ui_voice_sync(void *ctx)
{
    (void)ctx;
}

void ime_ui_voice_sync_async(void *user_data)
{
    (void)user_data;
}

void ime_ui_voice_detach(void *ctx)
{
    (void)ctx;
}

lv_pinyin_ime_voice_tap_mode_t lv_pinyin_ime_voice_tap_mode(void)
{
    return LV_PINYIN_IME_VOICE_TAP_TOGGLE;
}

/* 组件公开 API 里被 UI 直接调用的那几个（键表 / 按键回调） */
lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_of(lv_obj_t *ime)
{
    (void)ime;
    return NULL;
}

void lv_pinyin_ime_voice_key_pressed(lv_obj_t *ime)
{
    (void)ime;
}

void lv_pinyin_ime_voice_key_released(lv_obj_t *ime)
{
    (void)ime;
}

void lv_pinyin_ime_voice_key_cancelled(lv_obj_t *ime)
{
    (void)ime;
}

bool lv_pinyin_ime_voice_supported(void)
{
    return false;
}
