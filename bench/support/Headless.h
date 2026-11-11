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

#include "render/Painter.h"

namespace bench::Headless {

// TIV_RENDERER=own draws through Vulkan, else through the SDL renderer the app uses.
[[nodiscard]] bool own();

// Made on first use. Both backends draw on a hidden window. Null when none can be made.
[[nodiscard]] tiv::Painter *painter();

// Draws an empty frame, which submits the uploads waiting.
void present();


void shutdown();

}


#endif //TIV_BENCH_SUPPORT_HEADLESS_H
