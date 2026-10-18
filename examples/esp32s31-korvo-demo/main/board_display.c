/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-S31-Korvo-1 V1.1 display bring-up: an 800x480 RGB565 panel
 * (ST7262E43, on the ESP32-S3-LCD-EV-Board-SUB3 sub-board) plus a GT1151
 * capacitive touch controller.
 *
 * The pin map and the RGB timings below are the ones the official board
 * support package (espressif/esp32_s31_korvo_1) uses for this board; they are
 * re-created here instead of depending on the whole BSP so that a keyboard
 * demo does not pull in audio codecs, camera and SD card stacks. If the BSP
 * ever changes the panel wiring, this file has to be updated too.
 *
 * Wiring used here (ESP32-S31-WROOM-3 module pins):
 *
 *   RGB data     DB0..DB15 = GPIO8..GPIO19, GPIO33..GPIO36
 *   PCLK = 40   DE = 43   HSYNC = 44   VSYNC = 45   DISP = 38
 *   touch I2C    SDA = 0, SCL = 1 (shared with the ES8389 codec, unused here)
 *
 * The board has no LCD reset and no backlight enable pin: DISP is the only
 * control line, and the backlight is always on.
 */

#include "board_display.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt1151.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "korvo_display";

/* ---------------------------------------------------------------- hardware */

#define PIN_LCD_PCLK  40
#define PIN_LCD_DE    43
#define PIN_LCD_VSYNC 45
#define PIN_LCD_HSYNC 44
#define PIN_LCD_DISP  38

#define PIN_I2C_SDA   0
#define PIN_I2C_SCL   1

#define LCD_NUM_FB 1

/* LVGL renders into these buffers, esp_lcd copies them into the frame buffer.
 * 100 lines is 1/5 of the panel, which is a good compromise between the number
 * of flushes per screen update and the memory they cost. They live in PSRAM:
 * 2 x 800 x 100 x 2 B = 312 KB is more internal RAM than this demo should
 * take. */
#define LCD_DRAW_BUF_LINES 100

static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c_bus;

/* ------------------------------------------------------------------- panel */

static esp_err_t rgb_panel_init(esp_lcd_panel_handle_t *ret_panel)
{
    /* 18 MHz pixel clock, 800x480 at ~47 Hz. The porches are the panel's
     * datasheet values; pclk_active_neg is required because the ST7262E43
     * latches data on the falling edge. */
    const esp_lcd_rgb_timing_t timing = {
        .pclk_hz = 18 * 1000 * 1000,
        .h_res = BOARD_LCD_H_RES,
        .v_res = BOARD_LCD_V_RES,
        .hsync_pulse_width = 40,
        .hsync_back_porch = 40,
        .hsync_front_porch = 48,
        .vsync_pulse_width = 23,
        .vsync_back_porch = 32,
        .vsync_front_porch = 13,
        .flags.pclk_active_neg = true,
    };

    const esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = timing,
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = LCD_NUM_FB,
        /* DMA burst of 64 bytes: the maximum the RGB peripheral accepts, and
         * the setting the reference BSP ships with. */
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = PIN_LCD_DISP,
        .data_gpio_nums = {
            GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_11,
            GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
            GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19,
            GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36,
        },
        .flags.fb_in_psram = 1,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_cfg, ret_panel), TAG,
                        "RGB panel creation failed");

    esp_lcd_panel_reset(*ret_panel);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*ret_panel), TAG, "panel init failed");
    /* ST7262E43 does not need colour inversion and is mounted in landscape. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*ret_panel, false), TAG, "invert colour failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(*ret_panel, false), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*ret_panel, false, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*ret_panel, true), TAG, "disp on failed");

    ESP_LOGI(TAG, "RGB panel up: %dx%d, %d frame buffer(s), 18 MHz PCLK",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, LCD_NUM_FB);
    return ESP_OK;
}

/* ------------------------------------------------------------------- touch */

static esp_err_t touch_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t touch_init(lv_display_t *disp)
{
    ESP_RETURN_ON_ERROR(touch_i2c_init(), TAG, "I2C bus init failed");

    /* The GT1151 sits at 0x14 on the shared I2C bus; ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG()
     * carries the address and the register/parameter widths the controller uses. */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG,
                        "touch I2C panel IO creation failed");

    /* Same geometry and orientation as the panel: the sub-board's touch surface
     * is aligned with the display, so no mirroring or axis swap is needed. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt1151(tp_io, &tp_cfg, &tp), TAG,
                        "GT1151 init failed");

    /* Hand the controller to LVGL as the input device of this display. */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "adding the touch to LVGL failed");

    return ESP_OK;
}

/* -------------------------------------------------------------------- init */

esp_err_t board_display_init(lv_display_t **ret_disp)
{
    if (ret_disp != NULL) {
        *ret_disp = NULL;
    }

    /* LVGL itself is started by the port: it creates the rendering task and the
     * mutex that board_display_lock() uses. */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port init failed");

    ESP_RETURN_ON_ERROR(rgb_panel_init(&s_panel), TAG, "display init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = NULL,      /* the RGB panel is driven directly, no panel IO */
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,     /* the buffer is copied by the CPU, not DMA'd */
            .buff_spiram = true,   /* 312 KB is too much internal RAM */
            .swap_bytes = false,   /* RGB565 native order, no byte swap needed */
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = false,      /* no bounce buffer: the frame buffer is in PSRAM */
            .avoid_tearing = false,/* tearing is only visible on full-screen animation */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "adding the RGB display to LVGL failed");

    /* Touch is optional: a dead touch controller must not hide a working
     * panel, so only warn and continue. */
    esp_err_t touch_err = touch_init(disp);
    if (touch_err == ESP_OK) {
        ESP_LOGI(TAG, "GT1151 touch ready");
    } else {
        ESP_LOGW(TAG, "touch init failed (%s): the screen works, but there is no input",
                 esp_err_to_name(touch_err));
    }

    if (ret_disp != NULL) {
        *ret_disp = disp;
    }
    return ESP_OK;
}

bool board_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void board_display_unlock(void)
{
    lvgl_port_unlock();
}
