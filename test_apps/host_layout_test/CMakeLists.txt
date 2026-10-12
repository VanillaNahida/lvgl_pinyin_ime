cmake_minimum_required(VERSION 3.16)
project(host_layout_test C)

# 布局引擎的主机端验证：**不需要 LVGL 库**，只需要（可选的）LVGL 头来编译真实键表。
#
#   cmake -S test_apps/host_layout_test -B build/host_layout_test -G "MinGW Makefiles"
#   cmake --build build/host_layout_test
#   ./build/host_layout_test/host_layout_test
#
# 退出码非 0 表示有断言失败。IME_LAYOUT_TEST_KEYMAPS=OFF 时只跑合成数据的几何断言，
# 连 LVGL 头都不需要（ime_layout.c 本身不依赖 LVGL）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LVGL_DIR "${REPO_ROOT}/examples/k26_k9_demo/managed_components/lvgl__lvgl")

# 真实键表（src/ui/ime_keymaps.c）用 LV_SYMBOL_* 做特殊键的键面，所以需要 LVGL 的
# **头文件**（不需要链接它的任何 .c）。头不在就退化成纯几何测试，而不是让整个
# 测试编不过。
option(IME_LAYOUT_TEST_KEYMAPS "Also validate the component's real key tables" ON)
if(IME_LAYOUT_TEST_KEYMAPS AND NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(WARNING "LVGL headers not found at ${LVGL_DIR}; "
                    "building the synthetic-data tests only "
                    "(run a build of examples/k26_k9_demo once to fetch LVGL)")
    set(IME_LAYOUT_TEST_KEYMAPS OFF)
endif()

set(TEST_SRCS
    main.c
    "${REPO_ROOT}/src/ui/ime_layout.c"
)
set(TEST_INCLUDES "${REPO_ROOT}/src")
set(TEST_DEFINES "")

if(IME_LAYOUT_TEST_KEYMAPS)
    list(APPEND TEST_SRCS "${REPO_ROOT}/src/ui/ime_keymaps.c")
    # lvgl.h 需要一个 lv_conf.h：复用渲染测试那份（同一个 LVGL 检出）。
    list(APPEND TEST_INCLUDES
        "${LVGL_DIR}"
        "${LVGL_DIR}/src"
        "${REPO_ROOT}/test_apps/host_ui_render"
    )
    list(APPEND TEST_DEFINES IME_LAYOUT_TEST_KEYMAPS=1 LV_CONF_INCLUDE_SIMPLE)
endif()

add_executable(host_layout_test ${TEST_SRCS})
target_include_directories(host_layout_test PRIVATE ${TEST_INCLUDES})
if(TEST_DEFINES)
    target_compile_definitions(host_layout_test PRIVATE ${TEST_DEFINES})
endif()

if(MSVC)
    target_compile_options(host_layout_test PRIVATE /W4)
else()
    target_compile_options(host_layout_test PRIVATE -Wall -Wextra -Werror)
    if(IME_LAYOUT_TEST_KEYMAPS)
        # lvgl.h 自己会发一条 #warning（LV_COLOR_DEPTH 弃用），那是 LVGL 检出的配置，
        # 不是本测试的问题：只放行 #warning 这一类，别的警告仍然当错误。
        target_compile_options(host_layout_test PRIVATE -Wno-error=cpp)
    endif()
endif()
