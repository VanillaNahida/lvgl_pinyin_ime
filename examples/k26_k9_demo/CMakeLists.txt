cmake_minimum_required(VERSION 3.16)

# Both repository directories that contain components: the component at the
# root, and the vendored engine under src/third_party.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../src/third_party/libgooglepinyin"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(k26_k9_demo)
