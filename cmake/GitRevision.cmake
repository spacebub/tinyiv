# The header is written at build time so it cannot go stale. Run with -P, this file writes it.
if (CMAKE_SCRIPT_MODE_FILE)
    set(revision "")

    if (NOT TIV_RELEASE)
        set(revision "unknown")

        find_package(Git QUIET)

        if (GIT_FOUND)
            execute_process(
                    COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty=-dirty --abbrev=12
                    WORKING_DIRECTORY "${TIV_SOURCE_DIR}"
                    OUTPUT_VARIABLE described
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE result)

            if (result EQUAL 0 AND described)
                set(revision "${described}")
            endif ()
        endif ()
    endif ()

    set(content "#ifndef TIV_GIT_REVISION_H\n#define TIV_GIT_REVISION_H\n\n#define TIV_GIT_REVISION \"${revision}\"\n\n#endif //TIV_GIT_REVISION_H\n")

    if (EXISTS "${TIV_HEADER}")
        file(READ "${TIV_HEADER}" existing)
    else ()
        set(existing "")
    endif ()

    # Rewriting an identical header would rebuild everything that includes it.
    if (NOT existing STREQUAL content)
        file(WRITE "${TIV_HEADER}" "${content}")
    endif ()

    return()
endif ()

function(tiv_git_revision target)
    set(dir "${CMAKE_BINARY_DIR}/generated")
    set(header "${dir}/tiv_git_revision.h")

    set(command ${CMAKE_COMMAND}
            -DTIV_HEADER=${header}
            -DTIV_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DTIV_RELEASE=${TIV_RELEASE}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE})

    # Once now so the header is there to include, then every build, as a custom target is always out of date.
    # https://cmake.org/cmake/help/latest/command/add_custom_target.html
    file(MAKE_DIRECTORY "${dir}")
    execute_process(COMMAND ${command})

    add_custom_target(tiv_revision
            BYPRODUCTS "${header}"
            COMMAND ${command}
            COMMENT "Reading the git revision")

    add_dependencies(${target} tiv_revision)
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
