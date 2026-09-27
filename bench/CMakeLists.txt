include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)

set(TIV_BENCHMARK_VERSION v1.9.5)

tiv_cache_source(benchmark https://github.com/google/benchmark.git ${TIV_BENCHMARK_VERSION})

FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG ${TIV_BENCHMARK_VERSION}
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)

FetchContent_MakeAvailable(benchmark)

# tinyiv is an executable, so there is no library to link: its sources are read off the
# target, less the one holding main().
get_target_property(TIV_APP_SOURCES tinyiv SOURCES)
get_target_property(TIV_APP_DIR tinyiv SOURCE_DIR)

set(TIV_APP_REUSED "")

foreach (source IN LISTS TIV_APP_SOURCES)
    # Generated sources belong to the tinyiv build tree, so the bench makes its own.
    if (NOT source MATCHES "\\.cpp$" OR source STREQUAL "tinyiv.cpp" OR IS_ABSOLUTE ${source})
        continue()
    endif ()

    list(APPEND TIV_APP_REUSED "${TIV_APP_DIR}/${source}")
endforeach ()

add_executable(tiv_bench
        Main.cpp

        support/Corpus.cpp
        support/Corpus.h
        support/Headless.cpp
        support/Headless.h
        support/Memory.cpp
        support/Memory.h
        support/Register.h

        image/Bitmap_bench.cpp
        image/Bmp_bench.cpp
        image/Decode_bench.cpp
        image/Orient_bench.cpp
        image/Pyramid_bench.cpp

        render/Upload_bench.cpp

        services/Loader_bench.cpp

        app/Startup_bench.cpp

        ${TIV_APP_REUSED})

target_include_directories(tiv_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(tiv_bench PRIVATE ${TIV_COMPILE_FLAGS})

if (NOT WIN32)
    tiv_embed(tiv_bench icon128 ${CMAKE_SOURCE_DIR}/assets/tinyiv-128.png)
    tiv_embed(tiv_bench icon256 ${CMAKE_SOURCE_DIR}/assets/tinyiv-256.png)
endif ()
target_compile_definitions(tiv_bench PRIVATE VIPS_DISABLE_COMPAT)
target_link_libraries(tiv_bench PRIVATE SDL3::SDL3 PkgConfig::VIPS PkgConfig::CODECS benchmark::benchmark)

if (TIV_SANITIZE)
    target_compile_options(tiv_bench PRIVATE ${SANITIZER_FLAGS} -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)
    target_link_options(tiv_bench PRIVATE ${SANITIZER_FLAGS})
endif ()
