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
        image/Decode.cpp
        image/Decode.h
        image/Exif.cpp
        image/Exif.h
        image/Mapped.cpp
        image/Mapped.h
        image/Orient.cpp
        image/Orient.h
        image/Pyramid.cpp
        image/Pyramid.h
        image/Shrink.cpp
        image/Shrink.h
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

if (TIV_SANITIZE)
    # -Og folds frames, which misattributes sanitizer stacks.
    target_compile_options(tinyiv PRIVATE
            ${SANITIZER_FLAGS}
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -g)

    target_link_options(tinyiv PRIVATE ${SANITIZER_FLAGS})
endif ()
