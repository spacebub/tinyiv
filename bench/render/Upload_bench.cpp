// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "support/Headless.h"
#include "support/Memory.h"
#include "support/Register.h"

#include "app/App.h"
#include "image/Bitmap.h"
#include "image/Pyramid.h"
#include "render/Canvas.h"
#include "render/Painter.h"
#include "render/Tiles.h"
#include "render/gpu/VulkanPainter.h"
#include "view/Viewport.h"

namespace bench {
namespace {
constexpr int BATCH = 8;

std::shared_ptr<const tiv::Bitmap> grey(const int width, const int height) {
    tiv::Bitmap held = tiv::Bitmap::allocate(width, height);

    std::memset(held.data(), 0x80, held.bytes());

    return std::make_shared<const tiv::Bitmap>(std::move(held));
}

// Makes, fills and drops a batch of square tiles.
void Upload_static(benchmark::State &state, const int size) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    const std::shared_ptr<const tiv::Bitmap> pixels = grey(size, size);
    const int side = painter->max_tile();
    std::vector<tiv::Painter::Tile> tiles;

    for ([[maybe_unused]] const auto step : state) {
        for (int i = 0; i < BATCH; ++i) {
            for (int y = 0; y < size; y += side) {
                for (int x = 0; x < size; x += side) {
                    const tiv::Painter::Tile tile = painter->acquire_tile(side, side, tiv::Bitmap::Encoding::Srgb);
                    const std::size_t offset =
                            (static_cast<std::size_t>(y) * pixels->pitch()) + (static_cast<std::size_t>(x) * 4);

                    painter->upload(tile, pixels->all().subspan(offset).data(), pixels->pitch());
                    tiles.push_back(tile);
                }
            }
        }

        // Nothing is uploaded until a frame asks for it.
        if (painter->begin_frame({})) {
            painter->tile(tiles.back(), {.width = 1, .height = 1}, {.width = 1, .height = 1}, 1);
            painter->end_frame();
        }

        for (const tiv::Painter::Tile tile : tiles) {
            painter->release_tile(tile);
        }

        tiles.clear();
    }

    state.SetBytesProcessed(static_cast<std::int64_t>(size) * size * 4 * BATCH * state.iterations());
}

// The tiles a 4k view meets on a 128 MP image, and nothing else.
void Tiles_visible(benchmark::State &state) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    tiv::Tiles tiles(*painter, grey(13056, 9792));
    const tiv::Rect view{.x = 4000.0, .y = 3000.0, .width = 3840.0, .height = 2160.0};

    std::size_t uploaded = 0;

    for ([[maybe_unused]] const auto step : state) {
        std::size_t spent = tiles.upload(view, ~std::size_t{0}, 1);

        Headless::present();
        benchmark::DoNotOptimize(spent);
        uploaded = tiles.resident_bytes();
        tiles.evict_all();
    }

    state.SetBytesProcessed(static_cast<std::int64_t>(uploaded) * state.iterations());
    state.counters.insert_or_assign("mb_per_view",
                                    benchmark::Counter(static_cast<double>(uploaded) / (1024.0 * 1024.0)));
}

// All the VRAM the process holds, swapchain included. SDL reserves large blocks early, so a canvas
// adds little to the total it reports.
void report_vram(benchmark::State &state) {
    state.counters.insert_or_assign("vram_total_mb",
                                    benchmark::Counter(static_cast<double>(Memory::vram()) / (1024.0 * 1024.0)));
}

void frame(tiv::Painter &painter, tiv::Canvas &canvas, const tiv::Viewport &viewport) {
    if (painter.begin_frame({})) {
        canvas.draw(viewport);
        painter.end_frame();
    }
}

// A frame of draw calls at fit, with every tile resident.
void Canvas_draw(benchmark::State &state) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    tiv::Canvas canvas(*painter);
    tiv::Viewport viewport;
    const auto pyramid =
            std::make_shared<const tiv::Pyramid>(tiv::Pyramid::build(tiv::Bitmap::allocate(6656, 4992), 960, 540));

    viewport.set_area(3840.0, 2160.0);
    viewport.set_image(6656, 4992);
    canvas.show(pyramid, 1, 6656, 4992, 1);

    while (canvas.pending(viewport)) {
        canvas.upload(viewport, ~std::size_t{0});
    }

    for ([[maybe_unused]] const auto step : state) {
        frame(*painter, canvas, viewport);
    }

    report_vram(state);
    state.counters.insert_or_assign("vram_mb",
                                    benchmark::Counter(static_cast<double>(canvas.vram()) / (1024.0 * 1024.0)));

    if (const auto *vulkan = dynamic_cast<const tiv::gpu::VulkanPainter *>(painter); vulkan != nullptr) {
        state.counters.insert_or_assign(
                "pool_mb", benchmark::Counter(static_cast<double>(vulkan->resident_bytes()) / (1024.0 * 1024.0)));
    }
}

// Pans a 1:1 view across an image far larger than the VRAM budget. Each frame uploads within the
// budget of the app, so old tiles go as new ones come.
void Tiles_pan_at_budget(benchmark::State &state) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    constexpr int WIDTH = 13056;
    constexpr int HEIGHT = 9792;
    constexpr std::size_t BUDGET = std::size_t{256} * 1024 * 1024;
    tiv::Canvas canvas(*painter, BUDGET);
    tiv::Viewport viewport;
    const auto pyramid =
            std::make_shared<const tiv::Pyramid>(tiv::Pyramid::build(tiv::Bitmap::allocate(WIDTH, HEIGHT), 960, 540));

    viewport.set_area(3840.0, 2160.0);
    viewport.set_image(WIDTH, HEIGHT);
    viewport.zoom_to(1.0);
    canvas.show(pyramid, 1, WIDTH, HEIGHT, 1);

    double direction = -1.0;

    for ([[maybe_unused]] const auto step : state) {
        viewport.pan(direction * 480.0, direction * 270.0);

        const tiv::Rect moved = viewport.image_rect();

        // Viewport does not clamp a pan, so the view turns back at the edges itself.
        if (moved.x > 0.0 || moved.y > 0.0 || moved.x + moved.width < viewport.area_width()
            || moved.y + moved.height < viewport.area_height()) {
            viewport.pan(-direction * 480.0, -direction * 270.0);
            direction = -direction;
        }

        canvas.upload(viewport, tiv::App::UPLOAD_BUDGET);
        frame(*painter, canvas, viewport);
    }

    state.counters.insert_or_assign("vram_mb",
                                    benchmark::Counter(static_cast<double>(canvas.vram()) / (1024.0 * 1024.0)));
    report_vram(state);
}

// Time until a fresh 7000 by 4000 image shows whole and sharp at fit, a frame at a time.
void Frame_first(benchmark::State &state) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    constexpr int WIDTH = 7000;
    constexpr int HEIGHT = 4000;
    const auto pyramid =
            std::make_shared<const tiv::Pyramid>(tiv::Pyramid::build(tiv::Bitmap::allocate(WIDTH, HEIGHT), 960, 540));
    tiv::Viewport viewport;
    std::uint64_t image = 1;
    std::size_t frames = 0;

    viewport.set_area(3840.0, 2160.0);
    viewport.set_image(WIDTH, HEIGHT);
    viewport.fit();

    tiv::Canvas canvas(*painter);

    for ([[maybe_unused]] const auto step : state) {
        canvas.show(pyramid, ++image, WIDTH, HEIGHT, 1);

        for (bool more = true; more; more = canvas.pending(viewport)) {
            canvas.upload(viewport, tiv::App::UPLOAD_BUDGET);
            frame(*painter, canvas, viewport);
            ++frames;
        }

        state.PauseTiming();
        canvas.clear();
        state.ResumeTiming();
    }

    state.counters.insert_or_assign(
            "frames", benchmark::Counter(static_cast<double>(frames) / static_cast<double>(state.iterations())));
}

// An SDR image, then an HDR one, then the SDR one again, uploads only. The app on the SDL renderer also
// remakes the renderer at each switch, which this leaves out.
void Output_switch(benchmark::State &state) {
    tiv::Painter *painter = Headless::painter();

    if (painter == nullptr) {
        state.SkipWithMessage("no renderer");

        return;
    }

    const auto sdr =
            std::make_shared<const tiv::Pyramid>(tiv::Pyramid::build(tiv::Bitmap::allocate(3840, 2160), 960, 540));
    const auto hdr = std::make_shared<const tiv::Pyramid>(
            tiv::Pyramid::build(tiv::Bitmap::allocate(3840, 2160, tiv::Bitmap::Encoding::Pq), 960, 540));
    tiv::Viewport viewport;

    viewport.set_area(3840.0, 2160.0);
    viewport.set_image(3840, 2160);
    viewport.fit();

    tiv::Canvas canvas(*painter);

    for ([[maybe_unused]] const auto step : state) {
        for (const auto &[image, pyramid] : {std::pair{1, sdr}, std::pair{2, hdr}, std::pair{1, sdr}}) {
            canvas.show(pyramid, static_cast<std::uint64_t>(image), 3840, 2160, 1);

            while (canvas.pending(viewport)) {
                canvas.upload(viewport, tiv::App::UPLOAD_BUDGET);
                frame(*painter, canvas, viewport);
            }
        }

        state.PauseTiming();
        canvas.clear();
        state.ResumeTiming();
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
    benchmark::RegisterBenchmark("Tiles_visible_4k/128MP", Tiles_visible)->Unit(benchmark::kMillisecond)->UseRealTime();
    benchmark::RegisterBenchmark("Canvas_draw/33MP", Canvas_draw)->Unit(benchmark::kMicrosecond)->UseRealTime();
    benchmark::RegisterBenchmark("Tiles_pan_at_budget", Tiles_pan_at_budget)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
    benchmark::RegisterBenchmark("Frame_first/112MB", Frame_first)->Unit(benchmark::kMillisecond)->UseRealTime();
    benchmark::RegisterBenchmark("Output_switch", Output_switch)->Unit(benchmark::kMillisecond)->UseRealTime();
}
}
