/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board bring-up for the ESP32-S31-Korvo-1 V1.1 + ESP32-S3-LCD-EV-Board-SUB3.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel resolution of the 4.3" SUB3 sub-board, in landscape orientation. */
#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

/**
 * Bring up the display stack.
 *
 * Starts the LVGL port (LVGL itself + its rendering task), the RGB panel, the
 * GT1151 touch controller and registers both with LVGL. Must be called once,
 * from app_main, before any lv_* call.
 *
 * @param[out] ret_disp  the LVGL display, may be NULL if the caller does not
 *                       need it
 * @return ESP_OK on success, an error if the panel or the LVGL port failed.
 *         A missing touch controller only logs a warning: the screen still
 *         works, so a broken touch path does not hide a working display.
 */
esp_err_t board_display_init(lv_display_t **ret_disp);

/**
 * Take the LVGL mutex.
 *
 * LVGL is not thread safe, and esp_lvgl_port renders from its own task, so
 * every lv_* call outside an LVGL event callback has to be wrapped in this.
 *
 * @param timeout_ms  0 = wait forever
 * @return true if the mutex was taken
 */
bool board_display_lock(uint32_t timeout_ms);

/** Release the LVGL mutex taken by board_display_lock(). */
void board_display_unlock(void);

#ifdef __cplusplus
}
#endif
