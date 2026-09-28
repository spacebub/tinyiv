find_program(TIV_CLANG_FORMAT clang-format)

if (NOT TIV_CLANG_FORMAT)
    return()
endif ()

file(GLOB_RECURSE TIV_FORMAT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/bench/*.h"
        "${CMAKE_SOURCE_DIR}/bench/*.cpp")

add_custom_target(format
        COMMAND ${TIV_CLANG_FORMAT} -i ${TIV_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting the sources"
        VERBATIM)

add_custom_target(format-check
        COMMAND ${TIV_CLANG_FORMAT} --dry-run --Werror ${TIV_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking the sources are formatted"
        VERBATIM)
