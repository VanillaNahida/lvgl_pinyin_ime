/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal XPT2046 touch driver.
 *
 * The controller shares the LCD SPI bus and is polled: the IRQ pin is left
 * unused on purpose, see the reference board ESP32_MusicPlayer_V4.
 *
 * Calibration maps raw 12 bit samples onto screen pixels; the endpoints are
 * kept in NVS so a calibration survives reboots. touch_cal.c drives the
 * on-screen calibration wizard, this file only stores the result.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attach the touch controller to the already initialised LCD SPI bus.
 *
 * Must be called after the LCD bus is up (LCD_HOST is SPI2_HOST).
 */
esp_err_t touch_xpt2046_init(void);

/**
 * Register an LVGL pointer input device.
 *
 * Must be called after lv_init().
 *
 * @param h_res  screen width in pixels, used to scale raw values
 * @param v_res  screen height in pixels
 * @return the created input device
 */
lv_indev_t *touch_xpt2046_register_indev(uint16_t h_res, uint16_t v_res);

/**
 * Read one raw sample, without applying any calibration.
 *
 * @param raw_x     receives the sample feeding the screen X axis
 * @param raw_y     receives the sample feeding the screen Y axis
 * @param pressure  receives the pressure value, may be NULL
 * @return false when the panel is not being touched or the read failed
 */
bool touch_xpt2046_read_raw(int16_t *raw_x, int16_t *raw_y, int16_t *pressure);

/**
 * Split a raw sample into the value used for each screen axis.
 *
 * The controller's two channels do not map 1:1 onto the screen axes of a
 * landscape screen, this is the single place that decides the assignment.
 */
void touch_xpt2046_split_axes(int16_t raw_x, int16_t raw_y, int16_t *axis_x, int16_t *axis_y);

/** Periodically log every sample, touched or not. Used for diagnostics. */
void touch_xpt2046_set_raw_stream(bool enable);

/**
 * Take one sample and log the raw SPI bytes plus both possible byte orderings.
 * Used to diagnose a misaligned read on new hardware.
 */
void touch_xpt2046_dump_transaction(void);

/** @return true when the raw value stream is enabled */
bool touch_xpt2046_get_raw_stream(void);

/** Read the calibration currently in use, screen pixels against raw values. */
void touch_xpt2046_get_calibration(int32_t *x_left, int32_t *x_right, int32_t *y_top, int32_t *y_bottom);

/**
 * Apply a calibration.
 *
 * Rejects values that fail the sanity check (out of range or too small a span)
 * and returns false without changing anything.
 */
bool touch_xpt2046_set_calibration(int32_t x_left, int32_t x_right, int32_t y_top, int32_t y_bottom);

/** Load the calibration from NVS. Keeps the factory defaults when there is none. */
bool touch_xpt2046_load_calibration(void);

/** Store the current calibration in NVS. */
bool touch_xpt2046_save_calibration(void);

/** Drop the stored calibration and fall back to the factory defaults. */
esp_err_t touch_xpt2046_erase_calibration(void);

/** @return true when the active calibration came from NVS instead of the defaults */
bool touch_xpt2046_use_stored_calibration(void);

#ifdef __cplusplus
}
#endif