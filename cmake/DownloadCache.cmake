set(TIV_DOWNLOAD_CACHE "${CMAKE_SOURCE_DIR}/.download-cache"
        CACHE PATH "Where fetched sources are kept, shared by every build tree. Empty fetches into each tree")

function(tiv_cache_source name repo tag)
    string(TOUPPER ${name} upper)
    string(TOLOWER ${name} lower)

    if (NOT TIV_DOWNLOAD_CACHE)
        return()
    endif ()

    # A path under the cache was set here on an earlier configure and would pin its tag forever.
    # Any other path is the user's own override.
    # https://cmake.org/cmake/help/latest/module/FetchContent.html#variable:FETCHCONTENT_SOURCE_DIR_%3CuppercaseName%3E
    if (FETCHCONTENT_SOURCE_DIR_${upper})
        string(FIND "${FETCHCONTENT_SOURCE_DIR_${upper}}" "${TIV_DOWNLOAD_CACHE}/" at)

        if (NOT at EQUAL 0)
            return()
        endif ()
    endif ()

    set(dir "${TIV_DOWNLOAD_CACHE}/${lower}-${tag}")

    # Written after the clone, so an interrupted one is not taken for finished.
    if (NOT EXISTS "${dir}/.cached")
        find_package(Git REQUIRED)
        message(STATUS "Caching ${name} ${tag} in ${dir}")

        file(REMOVE_RECURSE "${dir}")

        execute_process(
                COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch ${tag}
                --recurse-submodules --shallow-submodules ${repo} ${dir}
                RESULT_VARIABLE result)

        if (NOT result EQUAL 0)
            file(REMOVE_RECURSE "${dir}")
            message(FATAL_ERROR "Could not fetch ${name} ${tag} from ${repo}: ${result}")
        endif ()

        file(TOUCH "${dir}/.cached")
    endif ()

    set(FETCHCONTENT_SOURCE_DIR_${upper} "${dir}" CACHE PATH
            "When not empty, overrides where to find pre-populated content for ${name}" FORCE)
endfunction()
