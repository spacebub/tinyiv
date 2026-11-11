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
#include <string_view>
#include <utility>

#include <SDL3/SDL.h>

#include "support/Headless.h"

#include "render/Painter.h"
#include "render/SdlPainter.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/VulkanPainter.h"

namespace bench {
namespace {
constexpr int WIDTH = 3840;
constexpr int HEIGHT = 2160;

struct Session {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    std::unique_ptr<tiv::Painter> painter;
    bool tried = false;
};

Session &session() {
    static Session held;

    return held;
}
}

bool Headless::own() {
    const char *renderer = SDL_getenv("TIV_RENDERER");

    return renderer != nullptr && std::string_view(renderer) == "own";
}

tiv::Painter *Headless::painter() {
    Session &held = session();

    if (held.tried) {
        return held.painter.get();
    }

    held.tried = true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return nullptr;
    }

    // Both draw to a hidden window and present, so both pay for the swapchain.
    held.window = SDL_CreateWindow("tiv_bench", WIDTH, HEIGHT, SDL_WINDOW_HIDDEN | (own() ? SDL_WINDOW_VULKAN : 0));

    if (held.window != nullptr && own()) {
        std::string error;

        held.painter = tiv::gpu::VulkanPainter::create(held.window, &error);

        if (held.painter == nullptr) {
            std::println(stderr, "{}", error);
        }

        return held.painter.get();
    }

    if (held.window == nullptr) {
        return nullptr;
    }

    held.renderer = SDL_CreateRenderer(held.window, "gpu");

    if (held.renderer == nullptr) {
        held.renderer = SDL_CreateRenderer(held.window, nullptr);
    }

    if (held.renderer != nullptr) {
        SDL_SetRenderVSync(held.renderer, 0);
        held.painter = std::make_unique<tiv::SdlPainter>(held.renderer);
    }

    return held.painter.get();
}

void Headless::present() {
    tiv::Painter *drawn = painter();

    if (drawn != nullptr && drawn->begin_frame({})) {
        drawn->end_frame();
    }
}

void Headless::shutdown() {
    Session &held = session();

    held.painter = nullptr;

    if (held.renderer != nullptr) {
        SDL_DestroyRenderer(held.renderer);
        held.renderer = nullptr;
    }

    if (held.window != nullptr) {
        SDL_DestroyWindow(held.window);
        held.window = nullptr;
    }

    if (held.tried) {
        SDL_Quit();
    }
}
}
