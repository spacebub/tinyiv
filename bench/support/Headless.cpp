// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <SDL3/SDL.h>

#include "support/Headless.h"

#include "render/Tiles.h"

namespace bench {
    namespace {
        SDL_Window *window = nullptr;
        SDL_Renderer *held = nullptr;
        bool tried = false;
    }

    SDL_Renderer *Headless::renderer() {
        if (tried) {
            return held;
        }

        tried = true;

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            return nullptr;
        }

        window = SDL_CreateWindow("tiv_bench", 640, 480, SDL_WINDOW_HIDDEN);

        if (window == nullptr) {
            return nullptr;
        }

        held = SDL_CreateRenderer(window, "gpu");

        if (held == nullptr) {
            held = SDL_CreateRenderer(window, nullptr);
        }

        if (held != nullptr) {
            SDL_SetRenderVSync(held, 0);
        }

        return held;
    }

    int Headless::max_texture() {
        SDL_Renderer *r = renderer();

        if (r == nullptr) {
            return tiv::Tiles::SIZE;
        }

        return static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(r), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, tiv::Tiles::SIZE));
    }

    void Headless::shutdown() {
        if (held != nullptr) {
            SDL_DestroyRenderer(held);
            held = nullptr;
        }

        if (window != nullptr) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        if (tried) {
            SDL_Quit();
        }
    }
}
