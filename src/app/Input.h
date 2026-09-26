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

#include <SDL3/SDL.h>

#include "view/Viewport.h"

namespace tiv {
    // The fixed scheme: left drag pans, right drag zooms, the wheel and the arrow keys walk the
    // folder, Home and End jump to its ends, Space plays or pauses an animation, [ and ] step
    // it back and forward a frame.
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
            TogglePlay,
            FrameBack,
            FrameForward,
            // The view changed.
            Redraw,
        };

        // Pixels of drag per doubling of the zoom, roughly.
        static constexpr double ZOOM_PER_PIXEL = 0.005;

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

        Drag _drag = Drag::None;
        double _pressY = 0.0;
        int _wheel = 0;
    };
}


#endif //TIV_APP_INPUT_H
