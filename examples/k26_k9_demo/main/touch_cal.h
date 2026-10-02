/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-screen touch calibration wizard, driven from the serial console.
 *
 * Why the serial port and not a button on the screen: when the current
 * calibration is way off, a button is exactly what you cannot hit. Entering
 * calibration has to be possible without the touch being usable.
 *
 * Sampling reads the raw controller values, never the current calibration, so
 * the wizard works no matter how wrong the old calibration is.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ask for a calibration run. Safe to call from any task: the wizard is built
 * by touch_cal_poll(), which runs on the LVGL task.
 */
void touch_cal_request_start(void);

/** Abort a running calibration (or a pending request). */
void touch_cal_request_cancel(void);

/** @return true while the wizard is on screen */
bool touch_cal_is_active(void);

/** Drive the wizard. Must be called from the LVGL task. */
void touch_cal_poll(void);

#ifdef __cplusplus
}
#endif