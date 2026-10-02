# lvgl_pinyin_ime: LVGL Chinese pinyin input method component.
#
# ---------------------------------------------------------------------------
# This directory is a COMPONENT, not a project: it has no project() call on
# purpose, so `idf.py build` here cannot work. Run idf.py from a project that
# consumes it instead:
#
#   examples/k26_k9_demo            flashable demo (ST7789 + XPT2046)
#   test_apps/idf_component_build   compile check, no hardware
#
#   cd examples/k26_k9_demo
#   idf.py set-target esp32s3
#   idf.py -p COM17 flash monitor
# ---------------------------------------------------------------------------
#
# src/third_party/libgooglepinyin is registered as a component by those projects
# through EXTRA_COMPONENT_DIRS, not by a recursive search from here.

set(LV_PINYIN_IME_SRCS
    "src/core/ime_alloc.c"
    "src/core/ime_engine_port.cpp"
    "src/core/ime_session.c"
    "src/core/ime_t9.c"
    "src/core/ime_trie.c"
    "src/core/ime_hash.c"
    "src/data/ime_dict.c"
    "src/data/ime_dict_vfs.c"
    "src/data/ime_font.c"
    "src/ui/lv_pinyin_ime.c"
    "src/ui/lv_pinyin_ime_cand.c"
    "src/ui/lv_pinyin_ime_kb.c"
    "src/ui/lv_pinyin_ime_style.c"
)

# Generated fonts (tools/gen_font.py). With FONT_SRC_EMBEDDED (the default) the
# C arrays are compiled into the application, so the glyphs are always present
# and nothing needs flashing. With FONT_SRC_PARTITION the binary image is flashed
# to the "font" partition instead (see docs/integration.md).
#
# This must happen before idf_component_register(), which is what actually
# consumes the source list.
if(CONFIG_LV_PINYIN_IME_FONT_SRC_EMBEDDED)
    foreach(_font
            "${CMAKE_CURRENT_LIST_DIR}/generated/lv_font_ime_20.c"
            "${CMAKE_CURRENT_LIST_DIR}/generated/lv_font_ime_16.c")
        if(NOT EXISTS "${_font}")
            message(FATAL_ERROR
                "lvgl_pinyin_ime: ${_font} is missing.\n"
                "Run: python tools/gen_charset.py && python tools/gen_font.py\n"
                "(or set CONFIG_LV_PINYIN_IME_FONT_SRC_PARTITION to flash the fonts instead)")
        endif()
        list(APPEND LV_PINYIN_IME_SRCS "${_font}")
    endforeach()
endif()

idf_component_register(
    SRCS ${LV_PINYIN_IME_SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl
        libgooglepinyin
    PRIV_REQUIRES
        esp_partition
        vfs
)

# src/ is private to the component. It is added with target_include_directories
# rather than PRIV_INCLUDE_DIRS because IDF's component validation rejects a
# private include directory that contains another component
# (src/third_party/libgooglepinyin).
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/third_party/libgooglepinyin/include"
)

set_target_properties(${COMPONENT_LIB} PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)

# Fallback copy of the system dictionary, linked into the application so that a
# board without a flashed "dict" partition still works (Kconfig source
# EMBEDDED, and the partition loader's fallback). It costs 1 MB of flash when
# the partition is present; set LV_PINYIN_IME_EMBED_DICT to OFF to skip it.
set(LV_PINYIN_IME_EMBED_DICT "ON" CACHE BOOL "Link dict_pinyin.dat into the application")
if(LV_PINYIN_IME_EMBED_DICT AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/data/dict/dict_pinyin.dat")
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/data/dict/dict_pinyin.dat" TEXT)
endif()

