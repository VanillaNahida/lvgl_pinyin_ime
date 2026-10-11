cmake_minimum_required(VERSION 3.16)
project(host_voice_test C)

# 语音输入协议层的主机端测试：不需要 LVGL、不需要 ESP-IDF、不需要网络。
# 组件在这一层刻意不依赖任何网络栈（网络由调用方通过回调接入），所以整条协议链
# —— 鉴权签名、请求帧、返回帧解析、动态修正 —— 都能在 PC 上逐字节验证。
#
# 用法：
#   cmake -S test_apps/host_voice_test -B build/host_voice_test -G Ninja
#   cmake --build build/host_voice_test
#   ./build/host_voice_test/host_voice_test

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(host_voice_test
    main.c
    "${REPO_ROOT}/src/voice/ime_voice_proto.c"
    "${REPO_ROOT}/src/voice/ime_voice_crypto.c"
)

target_include_directories(host_voice_test PRIVATE "${REPO_ROOT}/src")

if(MSVC)
    target_compile_options(host_voice_test PRIVATE /W4)
else()
    target_compile_options(host_voice_test PRIVATE -Wall -Wextra -Werror)
endif()
