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
        struct Session {
            SDL_Window *window = nullptr;
            SDL_Renderer *renderer = nullptr;
            bool tried = false;
        };

        Session &session() {
            static Session held;

            return held;
        }
    }

    SDL_Renderer *Headless::renderer() {
        Session &held = session();

        if (held.tried) {
            return held.renderer;
        }

        held.tried = true;

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            return nullptr;
        }

        held.window = SDL_CreateWindow("tiv_bench", 640, 480, SDL_WINDOW_HIDDEN);

        if (held.window == nullptr) {
            return nullptr;
        }

        held.renderer = SDL_CreateRenderer(held.window, "gpu");

        if (held.renderer == nullptr) {
            held.renderer = SDL_CreateRenderer(held.window, nullptr);
        }

        if (held.renderer != nullptr) {
            SDL_SetRenderVSync(held.renderer, 0);
        }

        return held.renderer;
    }

    int Headless::max_texture() {
        SDL_Renderer *r = renderer();

        if (r == nullptr) {
            return tiv::Tiles::SIZE;
        }

        return static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(r), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, tiv::Tiles::SIZE));
    }

    void Headless::shutdown() {
        Session &held = session();

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
