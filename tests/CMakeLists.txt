add_executable(tiv_tests
        Check.h
        Main.cpp

        render/Compare_test.cpp
        render/Painter_test.cpp)

tiv_reuse_app(tiv_tests)

target_include_directories(tiv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
