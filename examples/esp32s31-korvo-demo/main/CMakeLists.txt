# The demo is small on purpose: board_display.c brings up the panel and the
# touch controller, main.c only builds the LVGL screen.
#
# esp_lvgl_port and esp_lcd_touch_gt1151 are managed components declared in
# idf_component.yml. The component manager links them into main automatically,
# so they do not appear in REQUIRES (and their target names would have to be
# guessed as namespace__component anyway).

idf_component_register(
    SRCS
        "main.c"
        "board_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_lcd
        esp_timer
        lvgl
        lvgl_pinyin_ime
        nvs_flash
)

# main.c uses the IME font for its own Chinese captions (ime_font_big() /
# ime_font_small()). Those live in the component's private src/data, which is
# not exported, so add the component's src directory here. A real application
# should ship a font of its own instead.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../src"
)
