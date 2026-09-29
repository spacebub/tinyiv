add_executable(tinyiv
        tinyiv.cpp

        app/App.cpp
        app/App.h
        app/Config.cpp
        app/Config.h
        app/Input.cpp
        app/Input.h

        gallery/Folder.cpp
        gallery/Folder.h

        image/Animation.cpp
        image/Animation.h
        image/Bitmap.cpp
        image/Bitmap.h
        image/Channels.cpp
        image/Channels.h
        image/Exif.cpp
        image/Exif.h
        image/Mapped.cpp
        image/Mapped.h
        image/Orient.cpp
        image/Orient.h
        image/Pyramid.cpp
        image/Pyramid.h
        image/Reorient.cpp
        image/Reorient.h
        image/Shrink.cpp
        image/Shrink.h
        image/Simd.cpp
        image/Simd.h
        image/TileCache.cpp
        image/TileCache.h
        image/TileCodec.cpp
        image/TileCodec.h
        image/Tone.cpp
        image/Tone.h

        image/decode/Bmp.cpp
        image/decode/Bmp.h
        image/decode/Decode.cpp
        image/decode/Decode.h
        image/decode/Frames.cpp
        image/decode/GainMap.cpp
        image/decode/GainMap.h
        image/decode/Heif.cpp
        image/decode/Heif.h
        image/decode/Icon.cpp
        image/decode/Icon.h
        image/decode/Jpeg.cpp
        image/decode/Jpeg.h
        image/decode/JpegBands.cpp
        image/decode/JpegBands.h
        image/decode/JpegHandle.cpp
        image/decode/JpegHandle.h
        image/decode/Jxl.cpp
        image/decode/Jxl.h
        image/decode/Png.cpp
        image/decode/Png.h
        image/decode/Render.cpp
        image/decode/Stream.cpp
        image/decode/Support.cpp
        image/decode/Support.h
        image/decode/Svg.cpp
        image/decode/Svg.h
        image/decode/TiffBands.cpp
        image/decode/TiffBands.h
        image/decode/Vips.cpp
        image/decode/Vips.h
        image/decode/WebP.cpp
        image/decode/WebP.h

        render/Canvas.cpp
        render/Canvas.h
        render/PlayBar.cpp
        render/PlayBar.h
        render/StatusBar.cpp
        render/StatusBar.h
        render/Tiles.cpp
        render/Tiles.h

        services/Loader.cpp
        services/Loader.h
        services/Memory.cpp
        services/Memory.h
        services/Refiner.cpp
        services/Refiner.h

        view/Playback.cpp
        view/Playback.h
        view/Viewport.cpp
        view/Viewport.h)

target_include_directories(tinyiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tinyiv PRIVATE ${TIV_COMPILE_FLAGS})
# The libvips 7 compatibility macros rename plain identifiers such as error_exit, which libjpeg uses.
target_compile_definitions(tinyiv PRIVATE VIPS_DISABLE_COMPAT)
target_compile_definitions(tinyiv PRIVATE TIV_VERSION="${PROJECT_VERSION}")
tiv_git_revision(tinyiv)
target_link_libraries(tinyiv PRIVATE SDL3::SDL3 PkgConfig::VIPS PkgConfig::CODECS)

set_target_properties(tinyiv PROPERTIES WIN32_EXECUTABLE ON)

# Windows takes the window icon from the resources, everywhere else it is set on the window.
if (WIN32)
    target_sources(tinyiv PRIVATE
            resources/Win32/tinyiv.rc
            resources/Win32/compat.manifest)

    # WIN32_EXECUTABLE expects WinMain. Nothing defines one.
    if (MSVC)
        target_link_options(tinyiv PRIVATE /ENTRY:mainCRTStartup)
    endif ()
else ()
    tiv_embed(tinyiv icon128 ${CMAKE_SOURCE_DIR}/assets/tinyiv-128.png)
    tiv_embed(tinyiv icon256 ${CMAKE_SOURCE_DIR}/assets/tinyiv-256.png)
endif ()

if (TIV_SANITIZE)
    # -Og folds frames, which misattributes sanitizer stacks.
    target_compile_options(tinyiv PRIVATE
            ${SANITIZER_FLAGS}
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -g)

    target_link_options(tinyiv PRIVATE ${SANITIZER_FLAGS})
endif ()

include(GNUInstallDirs)

# vcpkg copies the DLLs beside the executable as it builds, so the install takes them from there.
# https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration#vcpkg_applocal_deps
if (WIN32)
    # Matches the exclusions against paths with forward slashes, as CMake 4.3 and later can.
    # https://cmake.org/cmake/help/latest/policy/CMP0207.html
    if (POLICY CMP0207)
        cmake_policy(SET CMP0207 NEW)
    endif ()

    install(TARGETS tinyiv RUNTIME_DEPENDENCY_SET tiv_runtime RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    install(RUNTIME_DEPENDENCY_SET tiv_runtime
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            POST_EXCLUDE_REGEXES "[/\\\\][Ss]ystem32[/\\\\]"
            DIRECTORIES $<TARGET_FILE_DIR:tinyiv>
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The Visual C++ runtime goes beside the executable, as a per user install cannot run the
    # redistributable, which needs elevation.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)

    # librsvg loads fontconfig, which reads its configuration from etc\fonts beside the bin
    # directory holding its DLL, or beside the DLL itself. Text draws through Windows fonts.
    set(TIV_FONTCONFIG_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/etc/fonts")

    if (EXISTS ${TIV_FONTCONFIG_DIR})
        install(DIRECTORY ${TIV_FONTCONFIG_DIR} DESTINATION etc)
    endif ()

    set(TIV_STAGE_DIR ${CMAKE_BINARY_DIR}/stage)
    file(TO_NATIVE_PATH ${CMAKE_SOURCE_DIR} TIV_ISS_SOURCE_DIR)
    file(TO_NATIVE_PATH ${TIV_STAGE_DIR} TIV_ISS_STAGE_DIR)
    file(TO_NATIVE_PATH ${CMAKE_BINARY_DIR} TIV_ISS_OUTPUT_DIR)
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/tinyiv.iss.in ${CMAKE_BINARY_DIR}/tinyiv.iss @ONLY)

    # A variable reference cannot spell parentheses, so the name goes in through another variable.
    set(program_files_x86 "ProgramFiles(x86)")
    find_program(TIV_ISCC ISCC
            PATHS
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 7"
            "$ENV{ProgramFiles}/Inno Setup 7"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
            "$ENV{${program_files_x86}}/Inno Setup 6"
            DOC "Inno Setup's command line compiler, for the installer target")

    if (TIV_ISCC)
        add_custom_target(installer
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${TIV_STAGE_DIR}
                COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --prefix ${TIV_STAGE_DIR}
                COMMAND ${TIV_ISCC} /Q ${CMAKE_BINARY_DIR}/tinyiv.iss
                DEPENDS tinyiv
                COMMENT "Compiling the setup"
                VERBATIM)
    endif ()
else ()
    install(TARGETS tinyiv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

# The Windows resources carry their own icon.
if (UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/assets/tinyiv.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

    foreach (size IN ITEMS 16 24 32 48 64 128 256 512)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/tinyiv-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME tinyiv.png)
    endforeach ()

    install(FILES ${CMAKE_SOURCE_DIR}/assets/tinyiv.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif ()
