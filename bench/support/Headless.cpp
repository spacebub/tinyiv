// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <memory>
#include <print>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include "support/Headless.h"

#include "render/Painter.h"
#include "render/gpu/VulkanPainter.h"

namespace bench {
namespace {
constexpr int WIDTH = 3840;
constexpr int HEIGHT = 2160;

struct Session {
    SDL_Window *window = nullptr;
    std::unique_ptr<tiv::gpu::VulkanPainter> painter;
    bool tried = false;
};

Session &session() {
    static Session held;

    return held;
}
}

tiv::gpu::VulkanPainter *Headless::painter() {
    Session &held = session();

    if (held.tried) {
        return held.painter.get();
    }

    held.tried = true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return nullptr;
    }

    // The window is hidden but still presents, so the benchmarks pay for the swapchain as the app does.
    held.window = SDL_CreateWindow("tiv_bench", WIDTH, HEIGHT, SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);

    if (held.window == nullptr) {
        return nullptr;
    }

    std::string error;

    held.painter = tiv::gpu::VulkanPainter::create(held.window, &error);

    if (held.painter == nullptr) {
        std::println(stderr, "{}", error);
    }

    return held.painter.get();
}

void Headless::present() {
    tiv::gpu::VulkanPainter *drawn = painter();

    if (drawn != nullptr && drawn->begin_frame({})) {
        drawn->end_frame();
    }
}

void Headless::shutdown() {
    Session &held = session();

    held.painter = nullptr;

    if (held.window != nullptr) {
        SDL_DestroyWindow(held.window);
        held.window = nullptr;
    }

    if (held.tried) {
        SDL_Quit();
    }
}
}
