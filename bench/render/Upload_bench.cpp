// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <benchmark/benchmark.h>

#include "support/Headless.h"
#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Pyramid.h"
#include "render/Canvas.h"
#include "render/Tiles.h"
#include "view/Viewport.h"

namespace bench {
    namespace {
        constexpr int BATCH = 8;

        std::shared_ptr<const tiv::Bitmap> grey(const int width, const int height) {
            tiv::Bitmap held = tiv::Bitmap::allocate(width, height);

            std::memset(held.data(), 0x80, held.bytes());

            return std::make_shared<const tiv::Bitmap>(std::move(held));
        }

        // Create, fill and drop a batch of square textures, the way a tile goes up.
        void Upload_static(benchmark::State &state, const int size) {
            SDL_Renderer *renderer = Headless::renderer();

            if (renderer == nullptr) {
                state.SkipWithMessage("no renderer");

                return;
            }

            const std::shared_ptr<const tiv::Bitmap> pixels = grey(size, size);
            std::vector<SDL_Texture *> textures;

            for ([[maybe_unused]] auto step : state) {
                for (int i = 0; i < BATCH; ++i) {
                    SDL_Texture *texture =
                            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);

                    SDL_UpdateTexture(texture, nullptr, pixels->data(), static_cast<int>(pixels->pitch()));
                    textures.push_back(texture);
                }

                // Nothing is uploaded until the GPU is asked for something.
                SDL_RenderTexture(renderer, textures.back(), nullptr, nullptr);
                SDL_RenderPresent(renderer);

                for (SDL_Texture *texture : textures) {
                    SDL_DestroyTexture(texture);
                }

                textures.clear();
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(size) * size * 4 * BATCH * state.iterations());
        }

        // The tiles a 4k view meets on a 128 MP image, and nothing else.
        void Tiles_visible(benchmark::State &state) {
            SDL_Renderer *renderer = Headless::renderer();

            if (renderer == nullptr) {
                state.SkipWithMessage("no renderer");

                return;
            }

            tiv::Tiles tiles(renderer, Headless::max_texture(), grey(13056, 9792));
            const tiv::Rect view{.x = 4000.0, .y = 3000.0, .width = 3840.0, .height = 2160.0};

            std::size_t uploaded = 0;

            for ([[maybe_unused]] auto step : state) {
                std::size_t spent = tiles.upload(view, ~std::size_t{0}, 1);

                SDL_RenderPresent(renderer);
                benchmark::DoNotOptimize(spent);
                uploaded = tiles.resident_bytes();
                tiles.evict_all();
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(uploaded) * state.iterations());
            state.counters.insert_or_assign("mb_per_view",
                                            benchmark::Counter(static_cast<double>(uploaded) / (1024.0 * 1024.0)));
        }

        // A frame of draw calls at fit, with every tile resident.
        void Canvas_draw(benchmark::State &state) {
            SDL_Renderer *renderer = Headless::renderer();

            if (renderer == nullptr) {
                state.SkipWithMessage("no renderer");

                return;
            }

            tiv::Canvas canvas(renderer, Headless::max_texture());
            tiv::Viewport viewport;
            auto pyramid = std::make_shared<const tiv::Pyramid>(
                    tiv::Pyramid::build(tiv::Bitmap::allocate(6656, 4992), 960, 540));

            viewport.set_area(3840.0, 2160.0);
            viewport.set_image(6656, 4992);
            canvas.show(pyramid, 1, 6656, 4992, 1);

            while (canvas.pending(viewport)) {
                canvas.upload(viewport, ~std::size_t{0});
            }

            for ([[maybe_unused]] auto step : state) {
                SDL_RenderClear(renderer);
                canvas.draw(viewport);
                SDL_RenderPresent(renderer);
            }
        }
    }

    void register_upload() {
        benchmark::RegisterBenchmark("Upload_static/1024", Upload_static, 1024)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        benchmark::RegisterBenchmark("Upload_static/2048", Upload_static, 2048)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        benchmark::RegisterBenchmark("Tiles_visible_4k/128MP", Tiles_visible)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        benchmark::RegisterBenchmark("Canvas_draw/33MP", Canvas_draw)->Unit(benchmark::kMicrosecond)->UseRealTime();
    }
}
