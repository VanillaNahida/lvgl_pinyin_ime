idf_component_register(
    SRCS
        "main.c"
        "touch_cal.c"
        "touch_xpt2046.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_uart
        esp_lcd
        esp_timer
        lvgl
        lvgl_pinyin_ime
        nvs_flash
)

# The demo caption ("LVGL中文智能拼音输入法") is Chinese and uses the IME font, so
# main.c includes the component-private header src/data/ime_font.h. Components do
# not export their private include directories, so add it here explicitly; a real
# application should ship a font of its own instead.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../src"
)
