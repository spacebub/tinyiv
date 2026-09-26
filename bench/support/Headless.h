// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_BENCH_SUPPORT_HEADLESS_H
#define TIV_BENCH_SUPPORT_HEADLESS_H


#include <SDL3/SDL.h>

namespace bench::Headless {

    // A renderer on a hidden window, created on first use with the driver the app uses. Null
    // when no renderer can be had.
    [[nodiscard]] SDL_Renderer *renderer();

    [[nodiscard]] int max_texture();

    void shutdown();

}


#endif //TIV_BENCH_SUPPORT_HEADLESS_H
