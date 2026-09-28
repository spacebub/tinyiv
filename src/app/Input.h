// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_INPUT_H
#define TIV_APP_INPUT_H


#include <cstdint>
#include <numbers>

#include <SDL3/SDL.h>

#include "view/Viewport.h"

namespace tiv {
    // The fixed scheme: left drag pans, right drag zooms, Up and Down zoom a step, 0 centres, 1 fits and
    // 2 shows the image at its own size, the wheel and Left and Right walk the folder, Home
    // and End jump to its ends, R reads the image from disk again, [ and ] turn the image
    // left and right, ; and ' flip it vertically and horizontally, Ctrl+S saves how it is
    // turned, S switches streaming mode, Space plays or pauses an animation, comma
    // and period step it a frame, Ctrl+H shows all of this.
    class Input {

    public:
        enum class Action : std::uint8_t {
            None,
            Quit,
            ToggleFullscreen,
            // Whole wheel notches are waiting in scroll().
            Scroll,
            First,
            Last,
            Reload,
            TurnLeft,
            TurnRight,
            FlipVertical,
            FlipHorizontal,
            Save,
            ToggleStream,
            ToggleHelp,
            TogglePlay,
            FrameBack,
            FrameForward,
            // The view changed.
            Redraw,
        };

        // Pixels of drag per doubling of the zoom, roughly.
        static constexpr double ZOOM_PER_PIXEL = 0.005;

        // Two key presses double the zoom.
        static constexpr double ZOOM_STEP = std::numbers::sqrt2;

        // Window units a cursor held on an edge moves back in before it runs free again.
        static constexpr float EDGE_RELEASE = 4.0F;

        // Mouse positions arrive in window units, density turns them into pixels.
        Action handle(const SDL_Event &event, float density, Viewport &viewport);

        // Positive is next, negative is previous. Clears what it returns.
        [[nodiscard]] int scroll();

    private:
        enum class Drag : std::uint8_t {
            None,
            Pan,
            Zoom,
        };

        Action key(const SDL_KeyboardEvent &event, Viewport &viewport);

        // A drag confines the cursor to the window, and relative mode carries it on while the cursor
        // rests on an edge. Wayland only reports motion past an edge to a locked cursor.
        void follow(const SDL_MouseMotionEvent &motion);
        void release(SDL_WindowID window);

        Drag _drag = Drag::None;
        bool _held = false;
        double _pressZoom = 1.0;
        double _zoomTravel = 0.0;
        int _wheel = 0;
    };
}


#endif //TIV_APP_INPUT_H
