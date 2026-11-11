// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Check.h"

#include "image/Bitmap.h"
#include "image/Pyramid.h"
#include "render/Canvas.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/VulkanPainter.h"
#include "view/Viewport.h"

namespace test {
namespace {
using tiv::Bitmap;

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;

// A view of the finest level draws from the coarsest when only that one is resident.
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

void register_canvas() {
    add("canvas/falls_back", falls_back);
}
}
