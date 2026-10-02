cmake_minimum_required(VERSION 3.16)
project(host_ui_render C CXX)

# Renders the IME widget with the real LVGL on the host and dumps a PPM, so the
# layout can be inspected without flashing a board. The engine and the fonts are
# stubbed: this test is about geometry, not about Chinese text.
#
#   cmake -S test_apps/host_ui_render -B build/host_ui_render -G Ninja
#   cmake --build build/host_ui_render
#   ./build/host_ui_render/host_ui_render out.ppm k26

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LVGL_DIR "${REPO_ROOT}/examples/k26_k9_demo/managed_components/lvgl__lvgl")
set(LGP "${REPO_ROOT}/src/third_party/libgooglepinyin")

if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "LVGL not found at ${LVGL_DIR}; build examples/k26_k9_demo once")
endif()

set(LGP_SRCS
    "${LGP}/src/dictio.cpp"
    "${LGP}/src/dictlist.cpp"
    "${LGP}/src/dicttrie.cpp"
    "${LGP}/src/lpicache.cpp"
    "${LGP}/src/matrixsearch.cpp"
    "${LGP}/src/mystdlib.cpp"
    "${LGP}/src/ngram.cpp"
    "${LGP}/src/pinyinime.cpp"
    "${LGP}/src/searchutility.cpp"
    "${LGP}/src/spellingtable.cpp"
    "${LGP}/src/spellingtrie.cpp"
    "${LGP}/src/splparser.cpp"
    "${LGP}/src/sync.cpp"
    "${LGP}/src/userdict.cpp"
    "${LGP}/src/utf16char.cpp"
)

add_library(googlepinyin_host STATIC ${LGP_SRCS})
target_include_directories(googlepinyin_host PUBLIC "${LGP}/include" "${LGP}/port")
target_compile_options(googlepinyin_host PRIVATE
    -include "${LGP}/port/pinyinime_compat.h"
    -w
)
target_compile_definitions(googlepinyin_host PUBLIC PINYINIME_RUNTIME_ONLY)

file(GLOB_RECURSE LVGL_SRCS "${LVGL_DIR}/src/*.c")
# Keep the core renderer, widgets and the software draw backend; drop the
# optional libraries and GPU/OS-specific backends that would need extra deps.
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/libs/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/drivers/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/others/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/espressif/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/nema_gfx/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/opengles/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/vg_lite/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/sdl/")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/src/draw/nanovg/")

add_library(lvgl_host STATIC ${LVGL_SRCS})
# lv_conf.h is picked up from this directory (LV_CONF_PATH-style include).
target_include_directories(lvgl_host PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
)
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl_host PRIVATE -w)

# A host build has no flash partitions, so src/data/ime_font.c always uses the
# embedded fonts here; the generated C files have to be linked either way. The
# option only decides whether the widget's labels are set to them (the default)
# or to LV_FONT_DEFAULT, which renders CJK as boxes but draws faster.
option(IME_HOST_REAL_FONT "Render with the generated IME fonts" ON)

add_executable(host_ui_render
    main.c
    "${REPO_ROOT}/src/ui/lv_pinyin_ime.c"
    "${REPO_ROOT}/src/ui/lv_pinyin_ime_cand.c"
    "${REPO_ROOT}/src/ui/lv_pinyin_ime_kb.c"
    "${REPO_ROOT}/src/ui/lv_pinyin_ime_style.c"
    "${REPO_ROOT}/src/core/ime_session.c"
    "${REPO_ROOT}/src/core/ime_t9.c"
    "${REPO_ROOT}/src/core/ime_trie.c"
    "${REPO_ROOT}/src/core/ime_alloc.c"
    "${REPO_ROOT}/src/core/ime_engine_port.cpp"
    "${REPO_ROOT}/src/data/ime_dict.c"
    "${REPO_ROOT}/src/data/ime_font.c"
    "${REPO_ROOT}/generated/lv_font_ime_20.c"
    "${REPO_ROOT}/generated/lv_font_ime_16.c"
)

if(IME_HOST_REAL_FONT)
    target_compile_definitions(host_ui_render PRIVATE IME_FONT_EMBEDDED)
endif()

# ime_dict.c calls ime_dict_self_test() during engine bring-up and the payload
# trim lives in ime_dict_vfs.c; compile that translation unit in host mode.
add_library(ime_vfs_host STATIC "${REPO_ROOT}/src/data/ime_dict_vfs.c")
target_include_directories(ime_vfs_host PUBLIC "${REPO_ROOT}/include" "${REPO_ROOT}/src")
target_compile_definitions(ime_vfs_host PUBLIC IME_VFS_HOST_TEST)
target_compile_options(ime_vfs_host PRIVATE -w)

target_include_directories(host_ui_render PRIVATE
    "${REPO_ROOT}/include"
    "${REPO_ROOT}/src"
)

target_link_libraries(host_ui_render PRIVATE lvgl_host googlepinyin_host ime_vfs_host)
