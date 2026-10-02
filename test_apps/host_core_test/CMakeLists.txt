cmake_minimum_required(VERSION 3.16)
project(host_core_test C CXX)

# Plain host build (MinGW/gcc + CMake) of the LVGL-independent part of the
# component. This is the fastest feedback loop for the engine port: it links the
# vendored libgooglepinyin exactly like the firmware does (decoder only, patch
# P0) and asserts ime_t9 / ime_session / engine behaviour.
#
# Usage:
#   cmake -S test_apps/host_core_test -B build/host_core_test -G Ninja
#   cmake --build build/host_core_test
#   ./build/host_core_test/host_core_test data/dict/dict_pinyin.dat

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LGP "${REPO_ROOT}/src/third_party/libgooglepinyin")

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
# Same shim the firmware uses: pthread macro surface without pthread. The host
# test also builds decoder-only, exactly like the firmware (patch P0).
target_compile_options(googlepinyin_host PRIVATE
    -include "${LGP}/port/pinyinime_compat.h"
    -w
)
target_compile_definitions(googlepinyin_host PUBLIC PINYINIME_RUNTIME_ONLY)

add_executable(host_core_test
    main.c
    "${REPO_ROOT}/src/core/ime_alloc.c"
    "${REPO_ROOT}/src/core/ime_hash.c"
    "${REPO_ROOT}/src/core/ime_t9.c"
    "${REPO_ROOT}/src/core/ime_trie.c"
    "${REPO_ROOT}/src/core/ime_session.c"
    "${REPO_ROOT}/src/core/ime_engine_port.cpp"
    "${REPO_ROOT}/src/data/ime_dict.c"
)

# The payload trim lives in ime_dict_vfs.c, which is ESP-IDF only. It is
# compiled in standalone mode here so the rule that stops a padded partition
# from breaking the dictionary load is covered by a test.
add_library(ime_vfs_host STATIC "${REPO_ROOT}/src/data/ime_dict_vfs.c")
target_include_directories(ime_vfs_host PUBLIC "${REPO_ROOT}/include" "${REPO_ROOT}/src")
target_compile_definitions(ime_vfs_host PUBLIC IME_VFS_HOST_TEST)
target_compile_options(ime_vfs_host PRIVATE -w)

target_include_directories(host_core_test PRIVATE
    "${REPO_ROOT}/include"
    "${REPO_ROOT}/src"
)

target_link_libraries(host_core_test PRIVATE googlepinyin_host ime_vfs_host)
