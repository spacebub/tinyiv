// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_STATUSBAR_H
#define TIV_RENDER_STATUSBAR_H


#include <string>

#include <SDL3/SDL.h>

#include "view/Viewport.h"

namespace tiv::StatusBar {

    // In pixels, for the display scale given.
    [[nodiscard]] int height(float scale);

    // How often a loading bar wants redrawing to keep moving.
    constexpr int LOADING_TICK_MS = 100;

    // The left text is cut to what fits beside the right one. While loading, a highlight
    // sweeps along the top edge and a spinner follows the right text.
    void draw(SDL_Renderer *renderer, const Rect &bar, float scale, const std::string &left, const std::string &right, bool loading);

    // A small dark box with one line of text, its bottom left corner at (x, y), with a
    // spinner after the text while loading.
    void badge(SDL_Renderer *renderer, double x, double y, float scale, const std::string &given, bool loading);

    // The badge, centred in the area, for a message in place of the image.
    void notice(SDL_Renderer *renderer, const Rect &area, float scale, const std::string &text);

}


#endif //TIV_RENDER_STATUSBAR_H
