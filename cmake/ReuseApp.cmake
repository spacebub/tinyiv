# The Vulkan renderer needs the headers only. SDL opens the loader at run time. The renderer fills Vulkan
# structs with designated initialisers and leaves pNext, flags and counts it does not use at zero.
function(tiv_vulkan target)
    target_include_directories(${target} SYSTEM PRIVATE ${TIV_VULKAN_INCLUDE})
    target_compile_definitions(${target} PRIVATE VK_NO_PROTOTYPES)

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(quiet -Wno-missing-designated-field-initializers)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(quiet -Wno-missing-field-initializers)
    else ()
        return()
    endif ()

    get_target_property(sources ${target} SOURCES)
    list(FILTER sources INCLUDE REGEX "render/gpu/[A-Za-z]+\\.cpp$")
    set_source_files_properties(${sources} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS ${quiet})
endfunction()

# tinyiv is an executable with no library to link. Programs built on the tinyiv code take the
# sources off the target, except the file with main().
function(tiv_reuse_app target)
    get_target_property(sources tinyiv SOURCES)
    get_target_property(dir tinyiv SOURCE_DIR)

    set(reused "")

    foreach (source IN LISTS sources)
        # Generated sources belong to the tinyiv build tree, so each program generates them again.
        if (NOT source MATCHES "\\.cpp$" OR source STREQUAL "tinyiv.cpp" OR IS_ABSOLUTE ${source})
            continue()
        endif ()

        list(APPEND reused "${dir}/${source}")
    endforeach ()

    target_sources(${target} PRIVATE ${reused})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE ${TIV_COMPILE_FLAGS})
    tiv_vulkan(${target})
    tiv_wayland(${target})
    # The libvips 7 compatibility macros rename plain identifiers such as error_exit, which libjpeg uses.
    target_compile_definitions(${target} PRIVATE VIPS_DISABLE_COMPAT TIV_VERSION="${PROJECT_VERSION}")

    if (NOT WIN32)
        tiv_embed(${target} icon128 ${CMAKE_SOURCE_DIR}/assets/tinyiv-128.png)
        tiv_embed(${target} icon256 ${CMAKE_SOURCE_DIR}/assets/tinyiv-256.png)
    endif ()

    # App.cpp includes the header tiv_git_revision writes for tinyiv.
    add_dependencies(${target} tiv_revision)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)

    target_link_libraries(${target} PRIVATE SDL3::SDL3-static PkgConfig::VIPS PkgConfig::CODECS)

    if (TIV_SANITIZE)
        target_compile_options(${target} PRIVATE ${SANITIZER_FLAGS} -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)
        target_link_options(${target} PRIVATE ${SANITIZER_FLAGS})
    endif ()
endfunction()
