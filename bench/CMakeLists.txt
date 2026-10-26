include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)

set(TIV_BENCHMARK_VERSION v1.10.0)

tiv_cache_source(benchmark https://github.com/google/benchmark.git ${TIV_BENCHMARK_VERSION})

FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG ${TIV_BENCHMARK_VERSION}
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)

FetchContent_MakeAvailable(benchmark)

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
        image/Pyramid_bench.cpp
        image/TileCache_bench.cpp

        image/decode/Bmp_bench.cpp
        image/decode/JpegBands_bench.cpp
        image/decode/Decode_bench.cpp

        render/Upload_bench.cpp

        services/Loader_bench.cpp

        app/Startup_bench.cpp)

tiv_reuse_app(tiv_bench)

target_include_directories(tiv_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tiv_bench PRIVATE benchmark::benchmark)
