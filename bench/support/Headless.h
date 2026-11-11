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

#include "render/gpu/VulkanPainter.h"

namespace bench::Headless {

// Made on first use, on a hidden window. Null when the window or the Vulkan device cannot be made.
[[nodiscard]] tiv::gpu::VulkanPainter *painter();

// Draws an empty frame, which submits the uploads waiting.
void present();


void shutdown();

}


#endif //TIV_BENCH_SUPPORT_HEADLESS_H
