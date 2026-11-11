// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <filesystem>
#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Headless.h"
#include "support/Register.h"

#include "gallery/Folder.h"
#include "image/decode/Decode.h"
#include "render/gpu/VulkanPainter.h"

namespace bench {
namespace {
void Startup_folder(benchmark::State &state, const std::filesystem::path &file) {
    for ([[maybe_unused]] const auto step : state) {
        tiv::Folder folder;

        if (!folder.open(file, tiv::Decode::suffixes())) {
            state.SkipWithError("cannot open");

            break;
        }

        benchmark::DoNotOptimize(folder.count());
    }
}

// SDL stays initialised, so only the window and renderer are measured.
void Startup_window(benchmark::State &state) {
    if (Headless::painter() == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    const bool own = Headless::own();

    for ([[maybe_unused]] const auto step : state) {
        SDL_Window *window =
                SDL_CreateWindow("tiv_bench", 1280, 800, SDL_WINDOW_HIDDEN | (own ? SDL_WINDOW_VULKAN : 0));

        if (own) {
            std::string error;
            std::unique_ptr<tiv::gpu::VulkanPainter> painter = tiv::gpu::VulkanPainter::create(window, &error);

            benchmark::DoNotOptimize(painter);
        } else {
            SDL_Renderer *renderer = SDL_CreateRenderer(window, "gpu");

            benchmark::DoNotOptimize(renderer);
            SDL_DestroyRenderer(renderer);
        }

        SDL_DestroyWindow(window);
    }
}
}

void register_startup() {
    benchmark::RegisterBenchmark("Startup_folder", Startup_folder, Corpus::file("small.jpg"))
            ->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("Startup_window", Startup_window)->Unit(benchmark::kMillisecond)->UseRealTime();
}
}
