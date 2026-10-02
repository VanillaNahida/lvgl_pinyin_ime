# libgooglepinyin 0.1.2 (upstream revision recorded in UPSTREAM_REVISION.txt).
#
# This directory is an ESP-IDF COMPONENT, not a project: run idf.py from
# examples/k26_k9_demo or test_apps/idf_component_build instead.
#
# Ported to ESP-IDF by lvgl_pinyin_ime; see UPSTREAM.md in this directory for
# the patch list and the rationale behind each patch.
#
# dictbuilder.cpp and utf16reader.cpp are the offline dictionary builder. They
# are kept in the tree for reference and for tools/, but are not compiled into
# the firmware: dict_pinyin.dat is produced on the host by tools/gen_dict.py
# (see patch P0 in include/dictdef.h).

set(LIBGOOGLEPINYIN_SRCS
    "src/dictio.cpp"
    "src/dictlist.cpp"
    "src/dicttrie.cpp"
    "src/lpicache.cpp"
    "src/matrixsearch.cpp"
    "src/mystdlib.cpp"
    "src/ngram.cpp"
    "src/pinyinime.cpp"
    "src/searchutility.cpp"
    "src/spellingtable.cpp"
    "src/spellingtrie.cpp"
    "src/splparser.cpp"
    "src/sync.cpp"
    "src/userdict.cpp"
    "src/utf16char.cpp"
)

idf_component_register(
    SRCS ${LIBGOOGLEPINYIN_SRCS}
    INCLUDE_DIRS
        "include"
        "port"
)

# P2/P4: route the POSIX-isms (pthread mutex macro surface) through a shim
# instead of editing upstream sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/port/pinyinime_compat.h"
)

# The vendored code is upstream code: keep it warning-clean without touching it
# (patch P7). Our own component still builds with its normal warning level.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# P0: the firmware only needs the decoder. Without this the model builder
# (dictbuilder.cpp / utf16reader.cpp plus the whole raw word list) would be
# pulled into the image.
target_compile_definitions(${COMPONENT_LIB} PRIVATE PINYINIME_RUNTIME_ONLY)

# P1: route malloc/free/calloc/realloc from the engine to the PSRAM-first
# allocator in lvgl_pinyin_ime.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc"
)
