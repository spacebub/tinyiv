cmake_minimum_required(VERSION 3.25)
project(tinyiv
        VERSION 1.0.0
        DESCRIPTION "A tiny, fast image viewer"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

option(TIV_SANITIZE "Build with the address and undefined sanitizers" OFF)
option(TIV_BENCHMARKS "Build the benchmark suite in bench/" OFF)

set(SANITIZER_FLAGS -fsanitize=address -fsanitize=undefined)

if (MSVC)
    set(TIV_COMPILE_FLAGS
            "/W4"
            "/WX"
            "/MP"
            "/GS"
            "$<$<CONFIG:RELEASE>:/O2;/GL;/Gw;/Gy>")

    add_link_options("$<$<CONFIG:RELEASE>:/LTCG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO;/DEBUG:NONE>")
else ()
    set(TIV_COMPILE_FLAGS
            "-Wall"
            "-Wextra"
            "-Werror"
            "-Wpedantic"
            "-fstack-protector-strong"
            "-fno-plt"
            "$<$<CONFIG:DEBUG>:-Og>"
            "$<$<CONFIG:RELEASE>:-O2;-ffunction-sections;-fdata-sections;-fvisibility=hidden>")

    add_link_options("$<$<CONFIG:RELEASE>:-Wl,--gc-sections;-Wl,--as-needed;-Wl,-O1;-s>")
endif ()

find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-shared)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips-cpp)
pkg_check_modules(CODECS REQUIRED IMPORTED_TARGET libwebp libjxl libjxl_threads libjpeg libpng)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(DownloadCache)
include(Embed)

add_subdirectory(src)

if (TIV_BENCHMARKS)
    add_subdirectory(bench)
endif ()
