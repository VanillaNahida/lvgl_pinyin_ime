/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Touch calibration wizard. Design follows the reference project's TouchCal:
 * four corner crosshairs, inset from the edges, sampled while the finger is
 * held down, then linearly extrapolated back to the screen borders.
 */

#include "touch_cal.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "touch_xpt2046.h"

static const char *TAG = "tcal";

/* Sample points sit inset from the borders: the outermost ring is often a dead
 * zone of the panel and a finger cannot reach a pixel edge anyway. The values
 * are extrapolated back to x = 0 / x = w-1 afterwards. */
#define CAL_MARGIN 20

/* Crosshair size in pixels. */
#define CAL_CROSS 28

#define CAL_POINTS 4

/* One point is accepted after this many samples (~0.2 s at the LVGL poll rate)
 * and a release is only believed after this many consecutive misses. */
#define CAL_MIN_SAMPLES     5
#define CAL_RELEASE_CONFIRM 2

/* How long the result stays on screen before closing. */
#define CAL_RESULT_MS 2500

typedef enum {
    CAL_IDLE = 0,
    CAL_WAIT,
    CAL_DONE,
} cal_state_t;

static cal_state_t s_state = CAL_IDLE;

static lv_obj_t *s_backdrop;
static lv_obj_t *s_panel;
static lv_obj_t *s_hint;
static lv_obj_t *s_sub;
static lv_obj_t *s_target;

static int16_t s_width;
static int16_t s_height;
static int16_t s_tgt_x[CAL_POINTS];
static int16_t s_tgt_y[CAL_POINTS];

static int s_index;
static int32_t s_raw_x[CAL_POINTS];
static int32_t s_raw_y[CAL_POINTS];
static bool s_touching;
static int32_t s_acc_x;
static int32_t s_acc_y;
static int s_acc_n;
static uint8_t s_miss;
static int64_t s_done_us;

/* Requests come from the console task, the wizard itself runs on the LVGL task. */
static volatile bool s_req_start;
static volatile bool s_req_cancel;

/* ------------------------------------------------------------------- UI */

static void cal_build_ui(void)
{
    /* Full screen backdrop: clickable but without a handler, so touches during
     * calibration are swallowed instead of reaching the widgets underneath.
     * Sampling itself goes through touch_xpt2046_read_raw(), a separate path. */
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_backdrop, 150, LV_PART_MAIN);
    lv_obj_set_clickable(s_backdrop, true);
    lv_obj_set_scrollable(s_backdrop, false);

    /* Hint panel goes in the middle: the four targets are in the corners, so
     * nothing overlaps and the finger does not cover the text. */
    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 236, 62);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x0B283D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, 240, LV_PART_MAIN);
    lv_obj_set_style_radius(s_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x1F6E93), LV_PART_MAIN);
    lv_obj_set_scrollable(s_panel, false);

    s_hint = lv_label_create(s_panel);
    lv_obj_set_width(s_hint, 220);
    lv_obj_set_pos(s_hint, 8, 6);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    s_sub = lv_label_create(s_panel);
    lv_obj_set_width(s_sub, 220);
    lv_obj_set_pos(s_sub, 8, 30);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x8FB8CE), LV_PART_MAIN);

    /* Crosshair: a container plus two bars and a centre dot; moving the
     * container is enough to point it at the next corner. */
    s_target = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_target);
    lv_obj_set_size(s_target, CAL_CROSS, CAL_CROSS);
    lv_obj_set_clickable(s_target, false);
    lv_obj_set_scrollable(s_target, false);

    lv_obj_t *h = lv_obj_create(s_target);
    lv_obj_remove_style_all(h);
    lv_obj_set_clickable(h, false);
    lv_obj_set_size(h, CAL_CROSS, 3);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0xFF4040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *v = lv_obj_create(s_target);
    lv_obj_remove_style_all(v);
    lv_obj_set_clickable(v, false);
    lv_obj_set_size(v, 3, CAL_CROSS);
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(0xFF4040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *c = lv_obj_create(s_target);
    lv_obj_remove_style_all(c);
    lv_obj_set_clickable(c, false);
    lv_obj_set_size(c, 7, 7);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
}

static void cal_close(void)
{
    if (s_backdrop) {
        /* Async: this can be reached from inside LVGL's event dispatch, and
         * deleting the object being dispatched would crash. */
        lv_obj_delete_async(s_backdrop);
        s_backdrop = NULL;
    }
    s_panel = NULL;
    s_hint = NULL;
    s_sub = NULL;
    s_target = NULL;
    s_state = CAL_IDLE;
}

static void cal_show_target(int index)
{
    if (s_target) {
        lv_obj_set_hidden(s_target, false);
        lv_obj_set_pos(s_target, s_tgt_x[index] - CAL_CROSS / 2, s_tgt_y[index] - CAL_CROSS / 2);
    }
    if (s_hint) {
        lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_label_set_text_fmt(s_hint, "Touch the cross centre (%d/%d)", index + 1, CAL_POINTS);
    }
    if (s_sub) {
        lv_label_set_text(s_sub, "hold ~0.2 s, then release");
    }
}

static void cal_show_result(const char *text, uint32_t color)
{
    if (s_target) {
        lv_obj_set_hidden(s_target, true);
    }
    if (s_hint) {
        lv_obj_set_style_text_color(s_hint, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_text(s_hint, text);
    }
    if (s_sub) {
        lv_label_set_text(s_sub, "");
    }
    s_state = CAL_DONE;
    s_done_us = esp_timer_get_time();
}

/* -------------------------------------------------------------- maths */

/* Linear extrapolation: raw value r1/r2 known at screen position x1/x2, find
 * the raw value at target. Needed because the sample points are inset while
 * the calibration wants the values at the screen borders. */
static int32_t cal_raw_at_screen(int32_t r1, int32_t r2, int32_t x1, int32_t x2, int32_t target)
{
    const int32_t denom = x2 - x1;

    if (denom == 0) {
        return r1;
    }
    return (int32_t)(r1 + (int64_t)(r2 - r1) * (target - x1) / denom);
}

static bool cal_axis_looks_swapped(int32_t span_x, int32_t span_y)
{
    return (abs((int)span_x) < 200 && abs((int)span_y) > 1000);
}

static void cal_finish(void)
{
    /* raw_x[]/raw_y[] already hold the value for each screen axis. */
    const int32_t xl = cal_raw_at_screen(s_raw_x[0], s_raw_x[1], s_tgt_x[0], s_tgt_x[1], 0);
    const int32_t xr = cal_raw_at_screen(s_raw_x[0], s_raw_x[1], s_tgt_x[0], s_tgt_x[1], s_width - 1);
    const int32_t yt = cal_raw_at_screen(s_raw_y[0], s_raw_y[2], s_tgt_y[0], s_tgt_y[2], 0);
    const int32_t yb = cal_raw_at_screen(s_raw_y[0], s_raw_y[2], s_tgt_y[0], s_tgt_y[2], s_height - 1);

    ESP_LOGI(TAG, "samples: (%d,%d) (%d,%d) (%d,%d) (%d,%d)", (int)s_raw_x[0], (int)s_raw_y[0],
             (int)s_raw_x[1], (int)s_raw_y[1], (int)s_raw_x[2], (int)s_raw_y[2], (int)s_raw_x[3],
             (int)s_raw_y[3]);
    ESP_LOGI(TAG, "extrapolated: x=%d..%d y=%d..%d", (int)xl, (int)xr, (int)yt, (int)yb);

    if (cal_axis_looks_swapped(xr - xl, yb - yt)) {
        ESP_LOGE(TAG, "x span is only %d while y span is %d", (int)(xr - xl), (int)(yb - yt));
        ESP_LOGW(TAG, "the two axes look exchanged: set TOUCH_SWAP_AXES to 1 in touch_xpt2046.c");
    }

    if (!touch_xpt2046_set_calibration(xl, xr, yt, yb)) {
        ESP_LOGE(TAG, "calibration failed, the samples look invalid, nothing was saved");
        cal_show_result("Calibration failed, press 'c' to retry", 0xFF8080);
        return;
    }

    if (touch_xpt2046_save_calibration()) {
        ESP_LOGI(TAG, "calibration done and stored in NVS");
        cal_show_result("Calibration stored", 0x7FE08A);
    } else {
        /* Applied but not persisted: say so instead of reporting success. */
        ESP_LOGW(TAG, "calibration applied but storing it in NVS failed");
        cal_show_result("Applied, but storing failed", 0xFFD070);
    }
}

/* --------------------------------------------------------------- wizard */

static void cal_begin(void)
{
    lv_display_t *disp = lv_display_get_default();

    s_width = (int16_t)lv_display_get_horizontal_resolution(disp);
    s_height = (int16_t)lv_display_get_vertical_resolution(disp);

    s_tgt_x[0] = CAL_MARGIN;
    s_tgt_x[1] = s_width - 1 - CAL_MARGIN;
    s_tgt_x[2] = CAL_MARGIN;
    s_tgt_x[3] = s_width - 1 - CAL_MARGIN;
    s_tgt_y[0] = CAL_MARGIN;
    s_tgt_y[1] = CAL_MARGIN;
    s_tgt_y[2] = s_height - 1 - CAL_MARGIN;
    s_tgt_y[3] = s_height - 1 - CAL_MARGIN;

    s_index = 0;
    s_touching = false;
    s_acc_x = 0;
    s_acc_y = 0;
    s_acc_n = 0;
    s_miss = 0;

    cal_build_ui();
    cal_show_target(0);
    s_state = CAL_WAIT;

    ESP_LOGI(TAG, "calibration started on a %dx%d screen: touch the centre of each cross",
             (int)s_width, (int)s_height);
    ESP_LOGI(TAG, "hold each point for about 0.2 s, send 'x' to abort");
}

void touch_cal_request_start(void)
{
    s_req_cancel = false;
    s_req_start = true;
}

void touch_cal_request_cancel(void)
{
    s_req_cancel = true;
    s_req_start = false;
}

bool touch_cal_is_active(void)
{
    return s_state != CAL_IDLE || s_req_start;
}

void touch_cal_poll(void)
{
    if (s_req_cancel) {
        s_req_cancel = false;
        if (s_state != CAL_IDLE) {
            cal_close();
            ESP_LOGI(TAG, "calibration aborted");
        } else {
            ESP_LOGI(TAG, "no calibration is running");
        }
        return;
    }

    if (s_req_start) {
        s_req_start = false;
        if (s_state != CAL_IDLE) {
            ESP_LOGW(TAG, "a calibration is already running, send 'x' to abort it first");
            return;
        }
        cal_begin();
        return;
    }

    if (s_state == CAL_IDLE) {
        return;
    }

    if (s_state == CAL_DONE) {
        if ((esp_timer_get_time() - s_done_us) >= (CAL_RESULT_MS * 1000)) {
            cal_close();
        }
        return;
    }

    int16_t raw_x = 0;
    int16_t raw_y = 0;
    bool hit = touch_xpt2046_read_raw(&raw_x, &raw_y, NULL);

    int16_t axis_x = 0;
    int16_t axis_y = 0;
    touch_xpt2046_split_axes(raw_x, raw_y, &axis_x, &axis_y);

    if (!s_touching) {
        if (hit) {
            s_touching = true;
            s_miss = 0;
            s_acc_x = axis_x;
            s_acc_y = axis_y;
            s_acc_n = 1;
            if (s_sub) {
                lv_label_set_text(s_sub, "keep holding, release to accept");
            }
        }
        return;
    }

    if (hit) {
        s_miss = 0;
        s_acc_x += axis_x;
        s_acc_y += axis_y;
        s_acc_n++;
        return;
    }

    /* A single empty read is not a release: the controller does drop samples. */
    s_miss++;
    if (s_miss < CAL_RELEASE_CONFIRM) {
        return;
    }

    s_touching = false;

    if (s_acc_n < CAL_MIN_SAMPLES) {
        ESP_LOGW(TAG, "point %d: only %d samples, touch it again", s_index + 1, s_acc_n);
        cal_show_target(s_index);
        return;
    }

    s_raw_x[s_index] = s_acc_x / s_acc_n;
    s_raw_y[s_index] = s_acc_y / s_acc_n;
    ESP_LOGI(TAG, "point %d: raw=(%d,%d) from %d samples", s_index + 1, (int)s_raw_x[s_index],
             (int)s_raw_y[s_index], s_acc_n);

    s_index++;
    if (s_index >= CAL_POINTS) {
        cal_finish();
        return;
    }
    cal_show_target(s_index);
}