cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    message(FATAL_ERROR "Select a verified device target or an explicit toolchain smoke target.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.1.0")
project(bottie_smoke)
