# Drives the whole app headless through scripted sittings, for profilers and sanitizers. See README.md here.
add_executable(tiv_harness
        Main.cpp

        Corpus.cpp
        Corpus.h
        Driver.cpp
        Driver.h
        Sandbox.cpp
        Sandbox.h
        Sittings.cpp
        Sittings.h
        Writers.cpp
        Writers.h)

tiv_reuse_app(tiv_harness)

target_include_directories(tiv_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
