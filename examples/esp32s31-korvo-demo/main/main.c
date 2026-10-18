/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl_pinyin_ime on the ESP32-S31-Korvo-1 V1.1.
 *
 * The board ships with the 4.3" ESP32-S3-LCD-EV-Board-SUB3 sub-board: 800x480
 * RGB565 display, GT1151 capacitive touch, a dual microphone array behind an
 * ES8389 codec. This demo uses the first two and puts the pinyin keyboard on
 * the bottom half of the screen; the microphones are left alone (see the note
 * about voice input in sdkconfig.defaults).
 *
 * The whole application is one screen:
 *
 *   +--------------------------------------------------------------+
 *   | title                                  [清空] [中/英] [26/9] |
 *   | status line (mode / language / commits)                      |
 *   | +----------------------------------------------------------+ |
 *   | | text area                                                | |
 *   | +----------------------------------------------------------+ |
 *   | hint                                                         |
 *   |               +--------------------------------+              |
 *   |               |  pinyin candidates + keyboard  |              |
 *   |               +--------------------------------+              |
 *   +--------------------------------------------------------------+
 *
 * The three buttons are there to show the programmatic API; the keyboard has
 * its own keys for the same switches. They call lv_pinyin_ime_show() because
 * tapping anywhere outside the keyboard dismisses it (that is the component's
 * default behaviour for real applications).
 */

#include "board_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#include "nvs_flash.h"

/* The captions in this file are Chinese, so they need the IME font: LVGL's
 * default font has no Hanzi and would draw every character as a box. This is
 * a component-private header, which is fine from inside this repository; a
 * real application should ship a font of its own. */
#include "data/ime_font.h"

static const char *TAG = "korvo_pinyin_ime_demo";

/* Height given to the keyboard, in pixels. The component turns this into a row
 * height, so it survives a switch between the 26-key and the 9-key layout.
 * The remaining 260 px of the 480 px panel hold the title, the status line,
 * the text area and the hint. */
#define IME_FIT_HEIGHT 250

#define COLOR_BG       0xEEF2F8
#define COLOR_CARD     0xFFFFFF
#define COLOR_TEXT     0x1F2933
#define COLOR_MUTED    0x6B7785
#define COLOR_ACCENT   0x2F6FED

static lv_obj_t *s_ime;
static lv_obj_t *s_status_label;
static uint32_t s_commit_count;

/* ----------------------------------------------------------------- widgets */

static void set_font(lv_obj_t *obj, const lv_font_t *font, lv_part_t part)
{
    /* A missing font is not fatal: the widget keeps LVGL's default font. */
    if (obj != NULL && font != NULL) {
        lv_obj_set_style_text_font(obj, font, part);
    }
}

static void update_status(void)
{
    if (s_status_label == NULL || s_ime == NULL) {
        return;
    }
    lv_label_set_text_fmt(s_status_label,
                          "模式：%s    语言：%s    已上屏：%u 次",
                          lv_pinyin_ime_get_mode(s_ime) == LV_PINYIN_IME_MODE_K9 ? "九键" : "26 键",
                          lv_pinyin_ime_get_lang(s_ime) == LV_PINYIN_IME_LANG_EN ? "英文" : "中文",
                          (unsigned)s_commit_count);
}

/* ------------------------------------------------------------------ events */

static void textarea_clicked_cb(lv_event_t *e)
{
    (void)e;
    /* After a confirm the keyboard may be hidden; tapping the text area brings
     * it back, which is also what an end user expects. */
    lv_pinyin_ime_show(s_ime);
}

static void ime_ready_cb(lv_event_t *e)
{
    (void)e;
    s_commit_count++;
    update_status();
    ESP_LOGI(TAG, "input confirmed, %u time(s) so far", (unsigned)s_commit_count);
}

static void clear_btn_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_user_data(e);

    if (ta != NULL) {
        lv_textarea_set_text(ta, "");
    }
    lv_pinyin_ime_reset(s_ime);
    lv_pinyin_ime_show(s_ime);
    update_status();
}

static void mode_btn_cb(lv_event_t *e)
{
    (void)e;

    const lv_pinyin_ime_mode_t next =
        lv_pinyin_ime_get_mode(s_ime) == LV_PINYIN_IME_MODE_K26 ? LV_PINYIN_IME_MODE_K9
                                                               : LV_PINYIN_IME_MODE_K26;
    lv_pinyin_ime_set_mode(s_ime, next);
    /* The click that reached this button also counts as "outside the keyboard"
     * and hides it; the user is still typing, so show it again. */
    lv_pinyin_ime_show(s_ime);
    update_status();
}

static void lang_btn_cb(lv_event_t *e)
{
    (void)e;

    const lv_pinyin_ime_lang_t next =
        lv_pinyin_ime_get_lang(s_ime) == LV_PINYIN_IME_LANG_EN ? LV_PINYIN_IME_LANG_CN
                                                              : LV_PINYIN_IME_LANG_EN;
    lv_pinyin_ime_set_lang(s_ime, next);
    lv_pinyin_ime_show(s_ime);
    update_status();
}

/* --------------------------------------------------------------------- ui */

static lv_obj_t *create_tool_button(lv_obj_t *parent, const char *text, int32_t x,
                                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 104, 44);
    lv_obj_set_pos(btn, x, 12);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    /* LVGL 9 draws the default button shadow; drop it, it looks blurry on a
     * 4.3" panel. */
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    set_font(label, ime_font_small(), LV_PART_MAIN);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /*
     * The keyboard goes in first: lv_pinyin_ime_create() is what loads the CJK
     * fonts, and the labels below want to use them.
     *
     * The fit height has to be set before the widget is created - it is read
     * while the rows are being built.
     */
    lv_pinyin_ime_set_fit_height(IME_FIT_HEIGHT);
    s_ime = lv_pinyin_ime_create(scr);
    if (s_ime != NULL) {
        lv_obj_align(s_ime, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else {
        ESP_LOGE(TAG, "lv_pinyin_ime_create() failed: the pinyin engine or the "
                      "dictionary partition is not available");
    }

    /* Title. Capped in width so a longer string can never run into the
     * toolbar on the right - dots appear instead of an overlap. */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-S31-Korvo-1 中文拼音输入法");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    set_font(title, ime_font_big(), LV_PART_MAIN);
    lv_obj_set_width(title, 424);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    /* Status line, kept up to date by update_status(). */
    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    set_font(s_status_label, ime_font_small(), LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 16, 46);

    /* Input box. The IME sets the CJK font on it (and on its placeholder) in
     * lv_pinyin_ime_attach(), because committed text is Chinese. */
    lv_obj_t *ta = lv_textarea_create(scr);
    lv_obj_set_size(ta, 768, 104);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 16, 80);
    lv_obj_set_style_bg_color(ta, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xD5DCE6), LV_PART_MAIN);
    lv_obj_set_style_pad_all(ta, 10, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_textarea_set_one_line(ta, false);
    lv_textarea_set_placeholder_text(ta, "点这里，用拼音输入中文…");

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "提示：点输入框唤起键盘；点空白处收起，点键盘上的确认键上屏。");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    set_font(hint, ime_font_small(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, 194);

    /* Toolbar, right to left. */
    create_tool_button(scr, "清空", 800 - 16 - 104, clear_btn_cb, ta);
    create_tool_button(scr, "中/英", 800 - 16 - 2 * 104 - 8, lang_btn_cb, NULL);
    create_tool_button(scr, "26/9", 800 - 16 - 3 * 104 - 16, mode_btn_cb, NULL);

    if (s_ime != NULL) {
        lv_pinyin_ime_attach(s_ime, ta);
        lv_obj_add_event_cb(ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(s_ime, ime_ready_cb, lv_pinyin_ime_event_ready(), NULL);
    }

    update_status();

    if (s_ime != NULL) {
        /* Bring-up aid: prints the geometry of the widget tree to the console. */
        lv_pinyin_ime_dump(s_ime);
    }
}

/* ------------------------------------------------------------------- entry */

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
    ESP_LOGI(TAG, "booting: ESP32-S31-Korvo-1 V1.1 + LCD-EV-Board-SUB3 (800x480 RGB, GT1151)");

    nvs_init();

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    ESP_LOGI(TAG, "display: %s", disp != NULL ? "registered with LVGL" : "not registered");

    /* LVGL is single threaded and the port renders from its own task, so the
     * UI is built - and inspected - while holding the mutex. */
    if (!board_display_lock(0)) {
        ESP_LOGE(TAG, "could not take the LVGL mutex, no UI was created");
        return;
    }
    ui_create();
    const int mode = s_ime != NULL ? (int)lv_pinyin_ime_get_mode(s_ime) : -1;
    const int lang = s_ime != NULL ? (int)lv_pinyin_ime_get_lang(s_ime) : -1;
    board_display_unlock();

    ESP_LOGI(TAG, "heap: internal %u B, PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "keyboard: %s, mode=%d lang=%d",
             s_ime != NULL ? "created" : "NOT created", mode, lang);

    /* Rendering runs in the esp_lvgl_port task; app_main only has to stay
     * alive (and would be the place to add a console, MQTT, ...). */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
