/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal demo host for the pinyin IME component.
 *
 * Display: ST7789 over SPI, 320 x 240 landscape, driven by the esp_lcd driver
 * shipped with ESP-IDF. Wiring follows the ESP32_MusicPlayer_V4 board:
 *
 *   SCLK = 12   MOSI = 11   MISO = 13   CS = 10   DC = 9   RST = 14
 *   BL   = 4    (backlight is active low)
 *   BGR colour order, inversion off, SPI clock 27 MHz
 *
 *   XPT2046 touch, sharing the same SPI bus: CS = 15, IRQ = 40 (IRQ unused,
 *   the controller is polled).
 *
 * Serial console commands (115200 8N1):
 *   c  start touch calibration        x  abort it
 *   t  print the active calibration   r  toggle the raw value stream
 *   d  dump one raw SPI transaction   n  erase the stored calibration
 *   h  help
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
/* For ime_font_big(): the app captions are Chinese and must use the IME font.
 * This is a component-private header, which is fine from inside this repo. */
#include "data/ime_font.h"
#include "touch_cal.h"
#include "touch_xpt2046.h"

static const char *TAG = "k26_k9_demo";

/* ---------------------------------------------------------------- hardware */

#define LCD_HOST SPI2_HOST

#define PIN_LCD_SCLK 12
#define PIN_LCD_MOSI 11
#define PIN_LCD_MISO 13
#define PIN_LCD_CS   10
#define PIN_LCD_DC   9
#define PIN_LCD_RST  14
#define PIN_LCD_BL   4

#define PIN_LCD_BL_ON_LEVEL 0 /* backlight is driven low to light up */

#define LCD_H_RES 320
#define LCD_V_RES 240

/* ST7789 tops out around 30 MHz on jumper wires; 27 MHz matches the reference
 * project and keeps margin. */
#define LCD_PIXEL_CLOCK_HZ (27 * 1000 * 1000)

/* Partial render buffer, must stay in internal RAM: the SPI DMA cannot read
 * from PSRAM. */
#define LCD_DRAW_BUF_LINES 40

#define CONSOLE_UART     UART_NUM_0
#define CONSOLE_RX_BYTES 256

static lv_color_t s_draw_buf[LCD_H_RES * LCD_DRAW_BUF_LINES];
static lv_display_t *s_lv_disp;

/* ------------------------------------------------------------------- LVGL */

static uint32_t demo_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Runs from the SPI ISR once the colour data has been shifted out. */
static bool lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_lv_disp) {
        lv_display_flush_ready(s_lv_disp);
    }
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

    /* esp_lcd takes exclusive end coordinates. */
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/* --------------------------------------------------------------- LCD init */

static void lcd_backlight_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_BL, PIN_LCD_BL_ON_LEVEL));
}

static esp_lcd_panel_handle_t lcd_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * (int)sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_trans_done_cb,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* Matches TFT_INVERSION_OFF of the reference setup. Flip to true if the
     * picture looks like a photo negative. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    /* The panel is natively 240 x 320; swap the axes for 320 x 240 landscape. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    /* Flip these two flags if the picture comes out mirrored or upside down. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    return panel;
}

static void lvgl_init(esp_lcd_panel_handle_t panel)
{
    lv_init();
    lv_tick_set_cb(demo_tick_cb);

    s_lv_disp = lv_display_create(LCD_H_RES, LCD_V_RES);

    /* The LCD controller is big endian: it wants the high byte of every RGB565
     * pixel first, while LVGL stores them little endian. Without this the
     * bytes are swapped and the colours come out wrong (green turns orange,
     * red turns blue) and anti-aliased text looks muddy. */
    lv_display_set_color_format(s_lv_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    lv_display_set_user_data(s_lv_disp, panel);
    lv_display_set_flush_cb(s_lv_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_lv_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
}

/* ---------------------------------------------------------- serial console */

static void console_print_help(void)
{
    ESP_LOGI(TAG, "commands: c = calibrate, x = abort, t = show calibration,");
    ESP_LOGI(TAG, "          r = toggle raw stream, d = dump SPI bytes,");
    ESP_LOGI(TAG, "          n = erase stored calibration, h = help");
}

static void console_dump_calibration(void)
{
    int32_t xl = 0;
    int32_t xr = 0;
    int32_t yt = 0;
    int32_t yb = 0;

    touch_xpt2046_get_calibration(&xl, &xr, &yt, &yb);
    ESP_LOGI(TAG, "calibration x=%d..%d y=%d..%d (%s)", (int)xl, (int)xr, (int)yt, (int)yb,
             touch_xpt2046_use_stored_calibration() ? "from NVS" : "factory default");

    int16_t raw_x = 0;
    int16_t raw_y = 0;
    int16_t z = 0;
    if (touch_xpt2046_read_raw(&raw_x, &raw_y, &z)) {
        ESP_LOGI(TAG, "live sample raw=(%d,%d) z=%d", (int)raw_x, (int)raw_y, (int)z);
    } else {
        ESP_LOGI(TAG, "live sample: not touched (hold the panel and send 't' again)");
    }
}

static void console_task(void *arg)
{
    (void)arg;

    uint8_t ch;

    while (1) {
        if (uart_read_bytes(CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(100)) != 1) {
            continue;
        }

        switch (ch) {
        case 'c':
            touch_cal_request_start();
            break;
        case 'x':
            touch_cal_request_cancel();
            break;
        case 't':
            console_dump_calibration();
            break;
        case 'r':
            touch_xpt2046_set_raw_stream(!touch_xpt2046_get_raw_stream());
            break;
        case 'd':
            touch_xpt2046_dump_transaction();
            break;
        case 'n':
            touch_xpt2046_erase_calibration();
            break;
        case 'h':
        case '?':
            console_print_help();
            break;
        case '\r':
        case '\n':
        case ' ':
            break;
        default:
            ESP_LOGW(TAG, "unknown command '%c', send 'h' for help", ch);
            break;
        }
    }
}

static void console_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART, &cfg));
    /* No TX ring buffer: ESP_LOG keeps writing through the ROM console. */
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BYTES, 0, 0, NULL, 0));

    /* The console task formats log lines and talks to NVS/SPI, give it some room. */
    xTaskCreate(console_task, "ime_console", 4096, NULL, 5, NULL);
}

/* -------------------------------------------------------------- app entry */

static void ime_ready_cb(lv_event_t *e)
{
    lv_obj_t *ime = lv_event_get_target_obj(e);
    ESP_LOGI(TAG, "IME ready event, hiding the keyboard");
    lv_pinyin_ime_hide(ime);
}

static void ta_clicked_cb(lv_event_t *e)
{
    lv_obj_t *ime = lv_event_get_user_data(e);
    lv_pinyin_ime_show(ime);
}

/* Proof of life for the touch panel: shows where LVGL thinks the last press
 * landed. If this label never changes, the problem is in the touch path; if it
 * changes but with silly coordinates, it is only the calibration. */
static lv_obj_t *s_tap_label;
static uint32_t s_tap_count;

static void show_tap_cb(lv_event_t *e)
{
    (void)e;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_tap_label == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_tap_count++;
    // lv_label_set_text_fmt(s_tap_label, "tap %u at (%d, %d)", (unsigned)s_tap_count, (int)p.x, (int)p.y);
    lv_label_set_text_fmt(s_tap_label, "LVGL中文智能拼音输入法");
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    lcd_backlight_init();
    esp_lcd_panel_handle_t panel = lcd_init();
    lvgl_init(panel);

    nvs_init();

    ESP_ERROR_CHECK(touch_xpt2046_init());
    touch_xpt2046_load_calibration();
    touch_xpt2046_register_indev(LCD_H_RES, LCD_V_RES);

    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ta = lv_textarea_create(scr);
    lv_obj_set_width(ta, LCD_H_RES - 20);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 10);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "输入文字...");

    s_tap_label = lv_label_create(scr);
    lv_label_set_text(s_tap_label, "LVGL中文智能拼音输入法");
    lv_obj_align(s_tap_label, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_event_cb(scr, show_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ta, show_tap_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *ime = lv_pinyin_ime_create(scr);
    if (ime == NULL) {
        /* The engine could not be loaded: there is no keyboard to show, so say
         * so loudly instead of leaving a screen that ignores every tap. */
        ESP_LOGE(TAG, "lv_pinyin_ime_create() failed: the pinyin engine is not "
                      "available, check the dictionary partition or the embedded copy");
    } else {
        lv_obj_align(ime, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_pinyin_ime_attach(ime, ta);
        /* Tapping the text area brings the keyboard back after the confirm key
         * has hidden it. */
        lv_obj_add_event_cb(ta, ta_clicked_cb, LV_EVENT_CLICKED, ime);

        /*
         * The caption under the input box is Chinese, so it needs the IME font:
         * the default LVGL font has no Hanzi and would draw it as boxes. This
         * comes after lv_pinyin_ime_create(), which is what loads the fonts.
         */
        if (ime_font_big() != NULL) {
            lv_obj_set_style_text_font(s_tap_label, ime_font_big(), LV_PART_MAIN);
        }

        lv_obj_add_event_cb(ime, ime_ready_cb, lv_pinyin_ime_event_ready(), NULL);
        /* Bring-up aid: prints the geometry of the widget tree to the console. */
        lv_pinyin_ime_dump(ime);
    }

    console_init();

    ESP_LOGI(TAG, "lvgl_pinyin_ime demo started, %dx%d ST7789 + XPT2046, mode=%d lang=%d",
             LCD_H_RES, LCD_V_RES, (int)lv_pinyin_ime_get_mode(ime), (int)lv_pinyin_ime_get_lang(ime));
    console_print_help();

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        touch_cal_poll();
        if (delay_ms > 10) {
            delay_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms ? delay_ms : 1));
    }
}