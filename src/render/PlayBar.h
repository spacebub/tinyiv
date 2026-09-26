// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_PLAYBAR_H
#define TIV_RENDER_PLAYBAR_H


#include <cstdint>

#include <SDL3/SDL.h>

#include "view/Viewport.h"

namespace tiv::PlayBar {

    enum class Part : std::uint8_t {
        None,
        Toggle,
        Track,
    };

    // In pixels, for the display scale given.
    [[nodiscard]] int height(float scale);

    // A translucent strip: a play or pause mark on the left, then a track filled up to the progress.
    void draw(SDL_Renderer *renderer, const Rect &bar, float scale, bool playing, double progress);

    // What lies under the point, in pixels.
    [[nodiscard]] Part hit(const Rect &bar, float scale, double x, double y);

    // The fraction of the track at x, clamped to it.
    [[nodiscard]] double seek(const Rect &bar, float scale, double x);

}


#endif //TIV_RENDER_PLAYBAR_H
