# HDR output on Wayland describes the surface through the colour management protocol.
if (UNIX AND NOT APPLE)
    find_program(TIV_WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.41)
    pkg_get_variable(TIV_WAYLAND_PROTOCOLS wayland-protocols pkgdatadir)
    set(TIV_WAYLAND ON)
endif ()

# Each target generates the code. tiv_reuse_app takes no generated sources.
function(tiv_wayland target)
    if (NOT TIV_WAYLAND)
        return()
    endif ()

    set(dir "${CMAKE_CURRENT_BINARY_DIR}/wayland")
    set(xml "${TIV_WAYLAND_PROTOCOLS}/staging/color-management/color-management-v1.xml")

    add_custom_command(
            OUTPUT "${dir}/color-management-v1.h" "${dir}/color-management-v1.c"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
            COMMAND ${TIV_WAYLAND_SCANNER} client-header "${xml}" "${dir}/color-management-v1.h"
            COMMAND ${TIV_WAYLAND_SCANNER} private-code "${xml}" "${dir}/color-management-v1.c"
            DEPENDS "${xml}"
            COMMENT "Generating the colour management protocol"
            VERBATIM)

    target_sources(${target} PRIVATE "${dir}/color-management-v1.c" "${dir}/color-management-v1.h")
    # Generated code is not ours to keep warning free.
    set_source_files_properties("${dir}/color-management-v1.c" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS -w)
    target_include_directories(${target} SYSTEM PRIVATE "${dir}")
    target_compile_definitions(${target} PRIVATE TIV_WAYLAND)

    # Version 1 of the protocol is bound, so the listeners leave the events of later versions unset.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(quiet -Wno-missing-designated-field-initializers)
    else ()
        set(quiet -Wno-missing-field-initializers)
    endif ()

    get_target_property(sources ${target} SOURCES)
    list(FILTER sources INCLUDE REGEX "render/WaylandColor\\.cpp$")
    set_source_files_properties(${sources} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS ${quiet})
    target_link_libraries(${target} PRIVATE PkgConfig::WAYLAND)
endfunction()
