cmake_minimum_required(VERSION 3.25)

# Read by the vcpkg toolchain in project(). The release only triplet builds the dependencies in
# half the time. A build tree configured on another triplet keeps it, host tools included, as
# the toolchain caches only the target triplet.
if (CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET x64-windows-release CACHE STRING "vcpkg triplet the dependencies are built for")
    set(VCPKG_HOST_TRIPLET x64-windows-release CACHE STRING "vcpkg triplet the build tools are built for")
endif ()

project(tinyiv
        VERSION 1.1.0
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
    # Release only dependencies are built on the release C runtime, and the C++ API of libvips
    # passes standard containers across the DLL boundary, so Debug builds use that runtime too.
    if (VCPKG_TARGET_TRIPLET MATCHES "-release$")
        set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
    endif ()

    set(TIV_COMPILE_FLAGS
            "/W4"
            "/WX"
            "/MP"
            "/GS"
            "/arch:AVX2"
            # jmp_buf is 16 byte aligned on x64, which pads the libjpeg error struct around it.
            "/wd4324"
            # libjpeg and libpng report errors by longjmp only, which unwinds on x64 MSVC.
            "/wd4611"
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
            "-march=x86-64-v3"
            "$<$<CONFIG:DEBUG>:-Og>"
            "$<$<CONFIG:RELEASE>:-O2;-ffunction-sections;-fdata-sections;-fvisibility=hidden>")

    add_link_options("$<$<CONFIG:RELEASE>:-Wl,--gc-sections;-Wl,--as-needed;-Wl,-O1;-s>")
endif ()

# pkg-config resolves one configuration, and with no build type the vcpkg toolchain puts its
# debug tree first, so every configuration would link the debug libraries.
get_property(TIV_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if (DEFINED VCPKG_INSTALLED_DIR AND EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug"
        AND (TIV_MULTI_CONFIG OR NOT CMAKE_BUILD_TYPE))
    message(FATAL_ERROR "The ${VCPKG_TARGET_TRIPLET} triplet has debug libraries, which pkg-config "
            "picks unless the build type is set. Use a release only triplet such as "
            "x64-windows-release, or a single configuration generator such as Ninja with "
            "CMAKE_BUILD_TYPE.")
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
