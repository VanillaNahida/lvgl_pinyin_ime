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
    "src/ui/ime_layout.c"
    "src/ui/ime_keymaps.c"
"src/ui/mic_icon.c"
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

# ---------------------------------------------------------------------------
# Voice input (iFlytek IAT), off by default.
#
# src/voice/ is only compiled when CONFIG_LV_PINYIN_IME_VOICE_ENABLE=y, so a
# project that never uses voice input carries none of its code and none of the
# FreeRTOS task/semaphore symbols it references.
#
# The module deliberately has NO network and NO audio dependency: the four
# callbacks in lv_pinyin_ime_voice_host_t are the application's job. See
# include/lvgl_pinyin_ime/lv_pinyin_ime_voice.h and docs/voice.md.
# ---------------------------------------------------------------------------
if(CONFIG_LV_PINYIN_IME_VOICE_ENABLE)
    list(APPEND LV_PINYIN_IME_SRCS
        "src/voice/ime_voice.c"
        "src/voice/ime_voice_crypto.c"
        "src/voice/ime_voice_proto.c"
        "src/voice/ime_voice_ui.c"
    )
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

# ---------------------------------------------------------------------------
# Kconfig 的值不需要在这里传给编译器。
#
# ★ 这一点踩过坑：ESP-IDF 会把**所有** Kconfig 符号都写进
#   build/config/sdkconfig.h（字符串带引号、bool 是 1、int 是数字），而那个头是
#   全工程强制包含的。所以 sdkconfig 里的 AppID/APIKey/端点/采样率在 C 里直接就是
#   CONFIG_LV_PINYIN_IME_VOICE_* ，不需要任何 target_compile_definitions。
#
#   早先这里为了"确保字符串也能传进去"加了一段 target_compile_definitions，结果和
#   sdkconfig.h 撞车：
#     - bool / int 用同一名字重复定义 → 触发 -Werror 的 "redefined"；
#     - 字符串因为两边内容完全一样，反而被编译器静默接受，于是这个错误只在
#       非字符串项上暴露出来，更容易看漏。
#
#   删掉之后只剩一个好处：少一层"同一个值有两个来源"的隐患。想改配置就去
#   menuconfig，唯一的真相在 sdkconfig.h。
#
# （src/voice/*.c 里仍保留 #ifndef CONFIG_... 的默认值，那是给主机端测试与
#   "宏没定义"的兜底用的，不影响固件行为。）
# ---------------------------------------------------------------------------

if(CONFIG_LV_PINYIN_IME_VOICE_ENABLE)
    # 只做一件与配置无关的事：确认语音源码真的在编译列表里（防止将来有人把上面那段
    # 的 list(APPEND) 删掉而没人发现）。
    list(FIND LV_PINYIN_IME_SRCS "src/voice/ime_voice.c" _voice_idx)
    if(_voice_idx EQUAL -1)
        message(FATAL_ERROR
            "lvgl_pinyin_ime: CONFIG_LV_PINYIN_IME_VOICE_ENABLE is on but "
            "src/voice/ime_voice.c is not in the source list")
    endif()
endif()

# src/ is private to the component. It is added with target_include_directories
# rather than PRIV_INCLUDE_DIRS because IDF's component validation rejects a
# private include directory that contains another component
# (src/third_party/libgooglepinyin).
#
# ★ src/ui 必须单独列出来：UI 的内部头 lv_pinyin_ime_internal.h 就在那，而
#   src/voice/ime_voice_ui.c 要 include 它（粘合层要读 ctx 的字段）。少了这一条，
#   语音关掉时编译毫无问题（没人 include 它），一开语音就报
#   "lv_pinyin_ime_internal.h: No such file or directory" —— 正是"默认配置能过、
#   打开开关才炸"的那类问题。
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/ui"
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

