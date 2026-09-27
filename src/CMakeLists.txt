add_executable(tinyiv
        tinyiv.cpp

        app/App.cpp
        app/App.h
        app/Input.cpp
        app/Input.h

        gallery/Folder.cpp
        gallery/Folder.h

        image/Animation.cpp
        image/Animation.h
        image/Bitmap.cpp
        image/Bitmap.h
        image/Bmp.cpp
        image/Bmp.h
        image/Decode.cpp
        image/Decode.h
        image/Exif.cpp
        image/Exif.h
        image/Icon.cpp
        image/Icon.h
        image/Mapped.cpp
        image/Mapped.h
        image/Orient.cpp
        image/Orient.h
        image/Pyramid.cpp
        image/Pyramid.h
        image/Shrink.cpp
        image/Shrink.h
        image/Simd.cpp
        image/Simd.h
        image/Svg.cpp
        image/Svg.h

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

install(TARGETS tinyiv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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
