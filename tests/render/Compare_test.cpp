// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "Check.h"

#include "image/Bitmap.h"
#include "image/Pyramid.h"
#include "render/Canvas.h"
#include "render/Painter.h"
#include "render/PlayBar.h"
#include "render/SdlPainter.h"
#include "render/StatusBar.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/VulkanPainter.h"
#include "view/Viewport.h"

namespace test {
namespace {
using tiv::Bitmap;
using tiv::Painter;

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;

// Red, green and blue of each pixel, rows packed.
using Pixels = std::vector<std::array<int, 3>>;

std::shared_ptr<const Bitmap> noise(const int width, const int height, const unsigned seed) {
    Bitmap held = Bitmap::allocate(width, height);

    for (int y = 0; y < height; ++y) {
        const std::span<std::uint8_t> row = held.row(y);

        for (std::size_t i = 0; i < row.size(); ++i) {
            // Blocks of eight, so filtering has edges and flats to work on.
            const std::size_t block = (i / 4 / 8) + (static_cast<std::size_t>(y) / 8 * 1000);

            row[i] = i % 4 == 3 ? 255
                                : static_cast<std::uint8_t>(((block * 2654435761U) + ((i % 4) * 97U) + seed) >> 3U);
        }
    }

    return std::make_shared<const Bitmap>(std::move(held));
}

// Draws the bars and a turned tile with the same calls on either painter.
void scene(Painter &painter, const std::array<Painter::Tile, 2> &tiles) {
    painter.tile(
            tiles.at(0), {.width = 100, .height = 80}, {.x = 10.5, .y = 30.25, .width = 137.0, .height = 109.6}, 1);
    painter.tile(tiles.at(1),
                 {.x = 10, .y = 5, .width = 60, .height = 60},
                 {.x = 170, .y = 40, .width = 90, .height = 90},
                 6);
    painter.tile(tiles.at(1), {.width = 100, .height = 80}, {.x = 20, .y = 150, .width = 70, .height = 56}, 2);
    tiv::StatusBar::draw(painter, {.width = WIDTH, .height = 26}, 1.0F, "12 / 340", "a name.jpg", "A to Z", false);
    tiv::StatusBar::badge(painter, 180.0, 230.0, 1.0F, "streaming", false);
    tiv::PlayBar::draw(painter, {.x = 0, .y = 200, .width = WIDTH, .height = 24}, 1.0F, false, 0.4);
}

Pixels through_sdl(const std::shared_ptr<const Bitmap> &first, const std::shared_ptr<const Bitmap> &second) {
    SDL_Window *window = SDL_CreateWindow("tiv_tests", WIDTH, HEIGHT, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(window, "gpu") : nullptr;
    Pixels pixels;

    if (renderer == nullptr) {
        SDL_DestroyWindow(window);

        return pixels;
    }

    {
        tiv::SdlPainter painter(renderer);
        std::array<Painter::Tile, 2> tiles{};

        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const Bitmap &bitmap = i == 0 ? *first : *second;

            tiles.at(i) = painter.acquire_tile(bitmap.width(), bitmap.height(), Bitmap::Encoding::Srgb);
            painter.upload(tiles.at(i), bitmap.data(), bitmap.pitch());
        }

        static_cast<void>(painter.begin_frame({.r = 40, .g = 40, .b = 40}));
        scene(painter, tiles);

        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        SDL_Surface *rgba = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : nullptr;

        if (rgba != nullptr && rgba->w == WIDTH && rgba->h == HEIGHT) {
            const std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t *>(rgba->pixels),
                                                      static_cast<std::size_t>(rgba->pitch) * HEIGHT);

            for (int y = 0; y < HEIGHT; ++y) {
                for (int x = 0; x < WIDTH; ++x) {
                    const std::span<const std::uint8_t> at =
                            bytes.subspan((static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->pitch))
                                                  + (static_cast<std::size_t>(x) * 4),
                                          4);

                    pixels.push_back({at[0], at[1], at[2]});
                }
            }
        }

        SDL_DestroySurface(rgba);
        SDL_DestroySurface(read);
        painter.end_frame();
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return pixels;
}

Pixels through_vulkan(const std::shared_ptr<const Bitmap> &first, const std::shared_ptr<const Bitmap> &second) {
    std::string error;
    const std::unique_ptr<tiv::gpu::VulkanPainter> painter =
            tiv::gpu::VulkanPainter::offscreen(WIDTH, HEIGHT, tiv::gpu::Swapchain::Output::Sdr, &error);
    Pixels pixels;

    if (painter == nullptr) {
        return pixels;
    }

    std::array<Painter::Tile, 2> tiles{};

    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const Bitmap &bitmap = i == 0 ? *first : *second;

        tiles.at(i) = painter->acquire_tile(bitmap.width(), bitmap.height(), Bitmap::Encoding::Srgb);
        painter->upload(tiles.at(i), bitmap.data(), bitmap.pitch());
    }

    if (!painter->begin_frame({.r = 40, .g = 40, .b = 40})) {
        return pixels;
    }

    scene(*painter, tiles);
    painter->end_frame();

    const std::vector<std::uint8_t> bytes = painter->read();

    for (std::size_t at = 0; at + 3 < bytes.size(); at += 4) {
        pixels.push_back({bytes.at(at + 2), bytes.at(at + 1), bytes.at(at)});
    }

    return pixels;
}

// The same scene through both. Differences are reported, and only a large one fails, since SDL
// may be the one that is off.
void matches_sdl() {
    const std::shared_ptr<const Bitmap> first = noise(100, 80, 5);
    const std::shared_ptr<const Bitmap> second = noise(100, 80, 6);
    const Pixels sdl = through_sdl(first, second);
    const Pixels own = through_vulkan(first, second);

    if (sdl.empty() || own.empty()) {
        skip("no SDL gpu renderer or no Vulkan device");

        return;
    }

    double total = 0.0;
    int largest = 0;
    std::size_t off = 0;

    for (std::size_t i = 0; i < sdl.size() && i < own.size(); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const int difference = std::abs(sdl.at(i).at(c) - own.at(i).at(c));

            total += difference;
            largest = std::max(largest, difference);
            off += difference > 8 ? 1 : 0;
        }
    }

    const double mean = total / static_cast<double>(sdl.size() * 3);
    const auto drawn = std::ranges::count_if(
            own, [](const std::array<int, 3> &pixel) { return pixel != std::array<int, 3>{40, 40, 40}; });

    check(std::cmp_greater(drawn, own.size() / 2), "the scene covers most of the frame");

    std::println("     against SDL: mean {:.3f}, largest {}, {} channels off by more than 8", mean, largest, off);
    check(mean < 1.0, std::format("the mean difference from SDL is {:.3f}", mean));
}

// With only the coarsest level resident, a view of the finest draws from it.
void falls_back() {
    std::string error;
    const std::unique_ptr<tiv::gpu::VulkanPainter> painter =
            tiv::gpu::VulkanPainter::offscreen(WIDTH, HEIGHT, tiv::gpu::Swapchain::Output::Sdr, &error);

    if (painter == nullptr) {
        skip(error);

        return;
    }

    Bitmap solid = Bitmap::allocate(4096, 4096);
    constexpr std::array<std::uint8_t, 4> COLOUR{200, 100, 50, 255};

    for (int y = 0; y < solid.height(); ++y) {
        const std::span<std::uint8_t> row = solid.row(y);

        for (std::size_t i = 0; i < row.size(); ++i) {
            row[i] = COLOUR.at(i % 4);
        }
    }

    const auto pyramid = std::make_shared<const tiv::Pyramid>(tiv::Pyramid::build(std::move(solid), 256, 256));
    tiv::Canvas canvas(*painter);
    tiv::Viewport viewport;

    viewport.set_area(WIDTH, HEIGHT);
    viewport.set_image(4096, 4096);
    viewport.zoom_to(1.0);
    canvas.show(pyramid, 1, 4096, 4096, 1);

    if (!painter->begin_frame({})) {
        return;
    }

    canvas.draw(viewport);
    painter->end_frame();

    const std::vector<std::uint8_t> bytes = painter->read();
    int wrong = 0;

    for (std::size_t at = 0; at + 3 < bytes.size(); at += 4) {
        const bool near = std::abs(bytes.at(at + 2) - 200) <= 1 && std::abs(bytes.at(at + 1) - 100) <= 1
                          && std::abs(bytes.at(at) - 50) <= 1;

        wrong += near ? 0 : 1;
    }

    check(wrong == 0, std::format("{} pixels are not the colour of the coarsest level", wrong));
}
}

void register_compare() {
    add("compare/matches_sdl", matches_sdl);
    add("compare/falls_back", falls_back);
}
}
