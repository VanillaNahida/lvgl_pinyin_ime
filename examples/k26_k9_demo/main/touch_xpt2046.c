/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * XPT2046 resistive touch controller, polled over the LCD SPI bus.
 *
 * Each conversion is read on its own (command byte, then the two bytes that
 * shift the result out), which is the framing every ADS7846/XPT2046 driver
 * uses. The "best two of three" averaging and the pressure formula follow Paul
 * Stoffregen's XPT2046_Touchscreen library (MIT), so the raw values stay
 * comparable with the ones the reference project calibrated.
 */

#include "touch_xpt2046.h"

#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "xpt2046";

/* ---------------------------------------------------------------- hardware */

/* Shares the LCD bus, only the chip select differs. */
#define TOUCH_HOST     SPI2_HOST
#define PIN_TOUCH_CS   15
#define PIN_TOUCH_MISO 13 /* must match PIN_LCD_MISO in main.c */
#define TOUCH_CLOCK_HZ 2000000 /* the controller is not happy much above 2 MHz */

/* Control bytes: start bit, channel, 12 bit mode, differential, PD = 01.
 * Same channels as the XPT2046_Touchscreen library uses, so the factory
 * calibration below keeps its meaning. */
#define TOUCH_CMD_X  0x91 /* A2A1A0 = 001 */
#define TOUCH_CMD_Y  0xD1 /* A2A1A0 = 101 */
#define TOUCH_CMD_Z1 0xB1 /* A2A1A0 = 011 */
#define TOUCH_CMD_Z2 0xC1 /* A2A1A0 = 100 */

/* Pressure below this counts as "not touched", same value as the XPT2046
 * library uses. */
#define TOUCH_Z_THRESHOLD 400

/* ------------------------------------------------------------- calibration */

/* Factory defaults for the 320 x 240 landscape screen.
 *
 * They come from the reference board's measured calibration (240 x 320
 * portrait, XPT2046 rotation 0):
 *
 *     portrait x: 3700 at 0,    320  at 239
 *     portrait y:  300 at 0,   3820 at 319
 *
 * ... rotated into landscape. The 0x91 channel ends up driving the screen X
 * axis and the 0xD1 channel the screen Y axis:
 *
 *     screen x (0x91 channel) = 300  .. 3820
 *     screen y (0xD1 channel) = 3775 .. 395   (inverted)
 *
 * Keep those channel assignments in sync with touch_xpt2046_read_raw(); getting
 * them the other way round makes the calibration wizard fail its span check.
 *
 * These are only a starting point: run the calibration wizard (serial 'c') on
 * a real panel, the result is stored in NVS and overrides them. */
#define TOUCH_DEFAULT_X_LEFT   300
#define TOUCH_DEFAULT_X_RIGHT  3820
#define TOUCH_DEFAULT_Y_TOP    3775
#define TOUCH_DEFAULT_Y_BOTTOM 395

/* Set to 1 if the two axes come out exchanged ("touching the left edge moves
 * the pointer up and down instead of left and right"). Calibration cannot fix
 * that, it would just fail the span check. */
#define TOUCH_SWAP_AXES 0

/* Sanity limits: the values are extrapolated from inset sample points, so they
 * may legitimately fall a bit outside 0..4095. */
#define TOUCH_CAL_ABS_LIMIT 2000
#define TOUCH_CAL_MIN_SPAN  200

/* NVS keys, keep the names stable or older calibrations stop being found. */
#define TOUCH_NVS_NAMESPACE "touch"
#define TOUCH_NVS_KEY_XL    "xl"
#define TOUCH_NVS_KEY_XR    "xr"
#define TOUCH_NVS_KEY_YT    "yt"
#define TOUCH_NVS_KEY_YB    "yb"
#define TOUCH_NVS_KEY_VER   "ver"

/* Bump this whenever the raw sample to screen coordinate path changes; a stored
 * calibration is only valid for the read path that produced it. Values written
 * by an older build are ignored, which is what we want: the early builds used a
 * different (and wrong) SPI framing, so their calibrations would make every tap
 * land in the wrong place. */
#define TOUCH_NVS_CAL_VERSION 3

/* ------------------------------------------------------------------ state */

static spi_device_handle_t s_dev;
static lv_indev_t *s_indev;
static uint16_t s_h_res;
static uint16_t s_v_res;
static bool s_ready;
static bool s_raw_stream;

static int32_t s_x_left = TOUCH_DEFAULT_X_LEFT;
static int32_t s_x_right = TOUCH_DEFAULT_X_RIGHT;
static int32_t s_y_top = TOUCH_DEFAULT_Y_TOP;
static int32_t s_y_bottom = TOUCH_DEFAULT_Y_BOTTOM;
static bool s_from_nvs;

/* Set when a reply does not look like a valid conversion, see touch_read_channel(). */
static bool s_framing_suspect;

/* --------------------------------------------------------------- internals */

/* One conversion: the command byte, then two clocks-only bytes that shift the
 * result out.
 *
 *   reply byte 1 = [busy = 0][R11 R10 R9 R8 R7 R6 R5]
 *   reply byte 2 = [R4 R3 R2 R1 R0][0 0 0]
 *
 * so the 12 bit result is (byte1 << 8 | byte2) >> 3. Reading one command at a
 * time keeps the window always at "the two bytes right behind the command";
 * a pipelined blob of several commands (as the Arduino library uses) makes the
 * reply bytes of neighbouring conversions overlap, which is impossible to
 * disambiguate from the outside and produced readings above 4095 on this
 * panel.
 *
 * @param raw  optional, receives the three reply bytes for diagnostics
 */
static bool touch_read_channel(uint8_t cmd, int16_t *value, uint8_t raw[3])
{
    const uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };

    spi_transaction_t trans = {
        .length = 3 * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(s_dev, &trans) != ESP_OK) {
        return false;
    }

    if (raw != NULL) {
        memcpy(raw, rx, sizeof(rx));
    }
    /* The top bit of the first reply byte is the converter's busy level and has
     * to be 0; a 1 there means the reply is not framed the way we think. */
    if ((rx[1] & 0x80) != 0) {
        s_framing_suspect = true;
    }

    /* rx[0] is shifted out while the command byte goes in, i.e. it still holds
     * the tail of whatever came before and carries no data. */
    *value = (int16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3);
    return true;
}

static int16_t best_two_avg(int16_t a, int16_t b, int16_t c);

/* Three conversions of the same channel, keep the two closest ones. */
static bool touch_read_axis(uint8_t cmd, int16_t *value)
{
    int16_t a = 0;
    int16_t b = 0;
    int16_t c = 0;

    if (!touch_read_channel(cmd, &a, NULL) || !touch_read_channel(cmd, &b, NULL) ||
        !touch_read_channel(cmd, &c, NULL)) {
        return false;
    }

    *value = best_two_avg(a, b, c);
    return true;
}

/* Average the two closest of three samples, dropping the outlier. */
static int16_t best_two_avg(int16_t a, int16_t b, int16_t c)
{
    int16_t ab = (a > b) ? (a - b) : (b - a);
    int16_t ac = (a > c) ? (a - c) : (c - a);
    int16_t bc = (b > c) ? (b - c) : (c - b);

    if (ab <= ac && ab <= bc) {
        return (int16_t)((a + b) >> 1);
    }
    if (ac <= ab && ac <= bc) {
        return (int16_t)((a + c) >> 1);
    }
    return (int16_t)((b + c) >> 1);
}

/* Raw value -> screen pixel. The interval may be reversed (hi < lo), which is
 * how one axis gets flipped. */
static int32_t scale_to_screen(int32_t raw, int32_t lo, int32_t hi, int32_t range)
{
    int32_t span = hi - lo;

    if (range <= 0 || span == 0) {
        return 0;
    }

    int32_t v = (int32_t)((int64_t)(raw - lo) * (range - 1) / span);
    if (v < 0) {
        v = 0;
    }
    if (v >= range) {
        v = range - 1;
    }
    return v;
}

static bool cal_sanity(int32_t xl, int32_t xr, int32_t yt, int32_t yb)
{
    const int32_t lo = -TOUCH_CAL_ABS_LIMIT;
    const int32_t hi = 4095 + TOUCH_CAL_ABS_LIMIT;

    if (xl < lo || xl > hi || xr < lo || xr > hi || yt < lo || yt > hi || yb < lo || yb > hi) {
        return false;
    }
    if (abs((int)(xr - xl)) < TOUCH_CAL_MIN_SPAN) {
        return false;
    }
    if (abs((int)(yb - yt)) < TOUCH_CAL_MIN_SPAN) {
        return false;
    }
    return true;
}

void touch_xpt2046_split_axes(int16_t raw_x, int16_t raw_y, int16_t *axis_x, int16_t *axis_y)
{
#if TOUCH_SWAP_AXES
    *axis_x = raw_y;
    *axis_y = raw_x;
#else
    *axis_x = raw_x;
    *axis_y = raw_y;
#endif
}

static void touch_sample_to_screen(int16_t raw_x, int16_t raw_y, int32_t *x, int32_t *y)
{
    int16_t axis_x = 0;
    int16_t axis_y = 0;

    touch_xpt2046_split_axes(raw_x, raw_y, &axis_x, &axis_y);
    *x = scale_to_screen(axis_x, s_x_left, s_x_right, s_h_res);
    *y = scale_to_screen(axis_y, s_y_top, s_y_bottom, s_v_res);
}

static void touch_raw_stream_log(int16_t raw_x, int16_t raw_y, int16_t z, bool hit)
{
    static int64_t last_us;

    int64_t now = esp_timer_get_time();
    if (now - last_us < 250000) { /* 4 lines per second is plenty */
        return;
    }
    last_us = now;

    int32_t x = 0;
    int32_t y = 0;
    touch_sample_to_screen(raw_x, raw_y, &x, &y);
    ESP_LOGI(TAG, "%s raw=(%d,%d) z=%d -> screen (%d,%d)", hit ? "touch" : "idle ",
             (int)raw_x, (int)raw_y, (int)z, (int)x, (int)y);
}

/* --------------------------------------------------------------- readings */

bool touch_xpt2046_read_raw(int16_t *raw_x, int16_t *raw_y, int16_t *pressure)
{
    if (!s_ready) {
        return false;
    }

    int16_t z1 = 0;
    int16_t z2 = 0;
    int16_t ch_x = 0;
    int16_t ch_y = 0;

    /* Z first: it is what decides whether the panel is being pressed at all. */
    if (!touch_read_channel(TOUCH_CMD_Z1, &z1, NULL) || !touch_read_channel(TOUCH_CMD_Z2, &z2, NULL) ||
        !touch_read_axis(TOUCH_CMD_X, &ch_x) || !touch_read_axis(TOUCH_CMD_Y, &ch_y)) {
        return false;
    }

    /* Touch pressure, same formula the Arduino library uses: with nothing
     * pressed Z1 sits near 0 and Z2 near full scale, so z stays small. */
    int32_t z = z1 + 4095 - z2;
    if (z < 0) {
        z = 0;
    }

    /* The 0x91 channel feeds the screen X axis, 0xD1 the screen Y axis; that
     * matches the factory calibration below (see the comment there). */
    if (s_raw_stream) {
        touch_raw_stream_log(ch_x, ch_y, (int16_t)z, z >= TOUCH_Z_THRESHOLD);
    }

    if (z < TOUCH_Z_THRESHOLD) {
        return false;
    }

    if (raw_x != NULL) {
        *raw_x = ch_x;
    }
    if (raw_y != NULL) {
        *raw_y = ch_y;
    }
    if (pressure != NULL) {
        *pressure = (int16_t)z;
    }
    return true;
}

void touch_xpt2046_dump_transaction(void)
{
    if (!s_ready) {
        return;
    }

    static const uint8_t cmds[4] = { TOUCH_CMD_Z1, TOUCH_CMD_Z2, TOUCH_CMD_X, TOUCH_CMD_Y };
    static const char *names[4] = { "Z1   ", "Z2   ", "X ch ", "Y ch " };

    s_framing_suspect = false;

    ESP_LOGI(TAG, "one conversion per channel; the busy bit must be 0 in every reply");
    for (int i = 0; i < 4; i++) {
        uint8_t raw[3] = { 0 };
        int16_t value = 0;
        if (!touch_read_channel(cmds[i], &value, raw)) {
            ESP_LOGE(TAG, "SPI transaction failed");
            return;
        }
        ESP_LOGI(TAG, "%s cmd=0x%02X reply=%02X %02X %02X  busy=%d  value=%d", names[i], cmds[i], raw[0],
                 raw[1], raw[2], (raw[1] >> 7) & 1, (int)value);
    }

    ESP_LOGI(TAG, "if framing is right: touch the panel and 'd' again, X ch and Y ch must move and Z2 must fall");
}

void touch_xpt2046_set_raw_stream(bool enable)
{
    s_raw_stream = enable;
    ESP_LOGI(TAG, "raw value stream %s", enable ? "on" : "off");
}

bool touch_xpt2046_get_raw_stream(void)
{
    return s_raw_stream;
}

/* ------------------------------------------------------------ LVGL indev */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    int16_t raw_x = 0;
    int16_t raw_y = 0;

    if (!touch_xpt2046_read_raw(&raw_x, &raw_y, NULL)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int32_t x = 0;
    int32_t y = 0;
    touch_sample_to_screen(raw_x, raw_y, &x, &y);

    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* ----------------------------------------------------------- calibration */

void touch_xpt2046_get_calibration(int32_t *x_left, int32_t *x_right, int32_t *y_top, int32_t *y_bottom)
{
    if (x_left) {
        *x_left = s_x_left;
    }
    if (x_right) {
        *x_right = s_x_right;
    }
    if (y_top) {
        *y_top = s_y_top;
    }
    if (y_bottom) {
        *y_bottom = s_y_bottom;
    }
}

bool touch_xpt2046_set_calibration(int32_t x_left, int32_t x_right, int32_t y_top, int32_t y_bottom)
{
    if (!cal_sanity(x_left, x_right, y_top, y_bottom)) {
        ESP_LOGW(TAG, "rejected calibration x=%d..%d y=%d..%d", (int)x_left, (int)x_right, (int)y_top,
                 (int)y_bottom);
        return false;
    }

    s_x_left = x_left;
    s_x_right = x_right;
    s_y_top = y_top;
    s_y_bottom = y_bottom;
    ESP_LOGI(TAG, "calibration applied: x=%d..%d y=%d..%d", (int)s_x_left, (int)s_x_right, (int)s_y_top,
             (int)s_y_bottom);
    return true;
}

bool touch_xpt2046_load_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TOUCH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored calibration (%s), using the factory defaults", esp_err_to_name(err));
        return false;
    }

    /* Refuse anything written for a different read path. */
    int32_t version = 0;
    err = nvs_get_i32(handle, TOUCH_NVS_KEY_VER, &version);
    if (err != ESP_OK || version != TOUCH_NVS_CAL_VERSION) {
        nvs_close(handle);
        ESP_LOGW(TAG, "stored calibration is from an older build (version %d, want %d), ignoring it",
                 (int)version, TOUCH_NVS_CAL_VERSION);
        ESP_LOGW(TAG, "run the calibration wizard ('c') again, it will overwrite the stale data");
        return false;
    }

    int32_t xl = s_x_left;
    int32_t xr = s_x_right;
    int32_t yt = s_y_top;
    int32_t yb = s_y_bottom;

    err = nvs_get_i32(handle, TOUCH_NVS_KEY_XL, &xl);
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, TOUCH_NVS_KEY_XR, &xr);
    }
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, TOUCH_NVS_KEY_YT, &yt);
    }
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, TOUCH_NVS_KEY_YB, &yb);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored calibration is incomplete (%s), using the factory defaults", esp_err_to_name(err));
        return false;
    }

    if (!cal_sanity(xl, xr, yt, yb)) {
        ESP_LOGW(TAG, "stored calibration is invalid, using the factory defaults");
        return false;
    }

    s_x_left = xl;
    s_x_right = xr;
    s_y_top = yt;
    s_y_bottom = yb;
    s_from_nvs = true;
    ESP_LOGI(TAG, "calibration loaded from NVS: x=%d..%d y=%d..%d", (int)xl, (int)xr, (int)yt, (int)yb);
    return true;
}

bool touch_xpt2046_save_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TOUCH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_i32(handle, TOUCH_NVS_KEY_VER, TOUCH_NVS_CAL_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, TOUCH_NVS_KEY_XL, s_x_left);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, TOUCH_NVS_KEY_XR, s_x_right);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, TOUCH_NVS_KEY_YT, s_y_top);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, TOUCH_NVS_KEY_YB, s_y_bottom);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving calibration failed: %s", esp_err_to_name(err));
        return false;
    }

    s_from_nvs = true;
    ESP_LOGI(TAG, "calibration saved to NVS: x=%d..%d y=%d..%d", (int)s_x_left, (int)s_x_right, (int)s_y_top,
             (int)s_y_bottom);
    return true;
}

esp_err_t touch_xpt2046_erase_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TOUCH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing the stored calibration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_x_left = TOUCH_DEFAULT_X_LEFT;
    s_x_right = TOUCH_DEFAULT_X_RIGHT;
    s_y_top = TOUCH_DEFAULT_Y_TOP;
    s_y_bottom = TOUCH_DEFAULT_Y_BOTTOM;
    s_from_nvs = false;
    ESP_LOGI(TAG, "stored calibration erased, factory defaults restored");
    return ESP_OK;
}

bool touch_xpt2046_use_stored_calibration(void)
{
    return s_from_nvs;
}

/* --------------------------------------------------------------------- API */

esp_err_t touch_xpt2046_init(void)
{
    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = TOUCH_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(TOUCH_HOST, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    /* TOUCH_HOST is an enum where SPI2_HOST conveniently equals 1, so print the
     * name as text instead of the number to avoid a misleading "SPI1". */
    ESP_LOGI(TAG, "XPT2046 attached to SPI2_HOST (id %d), CS=%d, %d Hz", TOUCH_HOST, PIN_TOUCH_CS,
             TOUCH_CLOCK_HZ);

    /* Self test: read the two position channels and the pressure channel once and
     * show what came back, so a wiring problem and a calibration problem can be
     * told apart immediately. */
    s_framing_suspect = false;

    uint8_t raw[3] = { 0 };
    int16_t ch_x = 0;
    int16_t ch_y = 0;
    int16_t z1 = 0;
    int16_t z2 = 0;

    if (!touch_read_channel(TOUCH_CMD_X, &ch_x, raw) ||
        !touch_read_channel(TOUCH_CMD_Y, &ch_y, NULL) ||
        !touch_read_channel(TOUCH_CMD_Z1, &z1, NULL) ||
        !touch_read_channel(TOUCH_CMD_Z2, &z2, NULL)) {
        ESP_LOGE(TAG, "SPI transaction failed");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "self test: X=%d Y=%d Z1=%d Z2=%d (first reply bytes %02X %02X %02X)", (int)ch_x, (int)ch_y,
             (int)z1, (int)z2, raw[0], raw[1], raw[2]);

    if (raw[0] == 0x00 && raw[1] == 0x00 && raw[2] == 0x00) {
        ESP_LOGW(TAG, "all reply bytes are 0x00: the controller is not driving MISO");
        ESP_LOGW(TAG, "check TOUCH_CS (GPIO%d), MISO (GPIO%d), 3.3V and GND", PIN_TOUCH_CS, PIN_TOUCH_MISO);
    } else if (s_framing_suspect) {
        ESP_LOGW(TAG, "the busy bit was 1, the reply framing is not what this driver expects");
        ESP_LOGW(TAG, "send 'd' and report the reply bytes");
    } else {
        ESP_LOGI(TAG, "controller answers and the framing looks sane");
        ESP_LOGI(TAG, "press the panel while sending 'r' to watch X/Y move and z rise");
    }

    return ESP_OK;
}

lv_indev_t *touch_xpt2046_register_indev(uint16_t h_res, uint16_t v_res)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "call touch_xpt2046_init() first");
        return NULL;
    }

    s_h_res = h_res;
    s_v_res = v_res;

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);

    return s_indev;
}