# Voice input sources are only compiled when the component's voice feature is on.
# Keeping them out of the default build means a project that never enables voice
# input does not need Wi-Fi, TLS or an I2S driver at all.
set(DEMO_SRCS
    "main.c"
    "touch_cal.c"
    "touch_xpt2046.c"
)

set(DEMO_REQUIRES
    esp_driver_gpio
    esp_driver_spi
    esp_driver_uart
    esp_lcd
    esp_timer
    lvgl
    lvgl_pinyin_ime
    nvs_flash
)

if(CONFIG_LV_PINYIN_IME_VOICE_ENABLE)
    list(APPEND DEMO_SRCS
        "net_wifi.c"
        "net_ws.c"
        "mic_i2s.c"
    )
    # esp_websocket_client is deliberately NOT repeated here: it comes from the
    # registry via main/idf_component.yml, and the component manager links managed
    # components into main automatically. Listing it would require guessing the
    # exact target name (namespace__component) for no benefit.
    list(APPEND DEMO_REQUIRES
        esp_driver_i2s        # PDM / standard I2S microphone (mic_i2s.c)
        esp_event
        esp_netif
        esp_wifi
    )
endif()

idf_component_register(
    SRCS ${DEMO_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${DEMO_REQUIRES}
)

# The demo caption ("LVGL中文智能拼音输入法") is Chinese and uses the IME font, so
# main.c includes the component-private header src/data/ime_font.h. Components do
# not export their private include directories, so add it here explicitly; a real
# application should ship a font of its own instead.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../src"
)
