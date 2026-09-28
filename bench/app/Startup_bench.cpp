// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>
#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Headless.h"
#include "support/Register.h"

#include "gallery/Folder.h"
#include "image/decode/Decode.h"

namespace bench {
    namespace {
        // Listing and sorting the folder around the opened file.
        void Startup_folder(benchmark::State &state, const std::filesystem::path &file) {
            for ([[maybe_unused]] auto step : state) {
                tiv::Folder folder;

                if (!folder.open(file, tiv::Decode::suffixes())) {
                    state.SkipWithError("cannot open");

                    break;
                }

                benchmark::DoNotOptimize(folder.count());
            }
        }

        // A window and a renderer, made and dropped. SDL itself stays up.
        void Startup_window(benchmark::State &state) {
            if (Headless::renderer() == nullptr) {
                state.SkipWithMessage("no renderer");

                return;
            }

            for ([[maybe_unused]] auto step : state) {
                SDL_Window *window = SDL_CreateWindow("tiv_bench", 1280, 800, SDL_WINDOW_HIDDEN);
                SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_GetRendererName(Headless::renderer()));

                benchmark::DoNotOptimize(renderer);
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(window);
            }
        }
    }

    void register_startup() {
        benchmark::RegisterBenchmark("Startup_folder", Startup_folder, Corpus::file("small.jpg"))->Unit(benchmark::kMicrosecond);
        benchmark::RegisterBenchmark("Startup_window", Startup_window)->Unit(benchmark::kMillisecond)->UseRealTime();
    }
}
