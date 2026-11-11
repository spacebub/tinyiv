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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Check.h"

#include "image/Bitmap.h"
#include "image/Orient.h"
#include "image/Tone.h"
#include "render/Font.h"
#include "render/Painter.h"
#include "render/gpu/Device.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/VulkanPainter.h"
#include "view/Viewport.h"

namespace test {
namespace {
using tiv::Bitmap;
using tiv::Colour;
using tiv::Painter;
using tiv::Rect;
using tiv::gpu::Swapchain;
using tiv::gpu::VulkanPainter;

constexpr int SIDE = 256;
constexpr Colour CLEAR{};

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

class Frame {

public:
    Frame(std::vector<std::uint8_t> bytes, const bool hdr) : _bytes(std::move(bytes)), _hdr(hdr) {}

    [[nodiscard]] bool empty() const { return _bytes.empty(); }

    [[nodiscard]] Rgba at(const int x, const int y) const {
        const std::size_t offset = ((static_cast<std::size_t>(y) * SIDE) + static_cast<std::size_t>(x)) * 4;
        const std::span<const std::uint8_t> pixel = std::span(_bytes).subspan(offset, 4);

        if (!_hdr) {
            return {.r = pixel[2], .g = pixel[1], .b = pixel[0], .a = pixel[3]};
        }

        const std::uint32_t word = static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8U)
                                   | (static_cast<std::uint32_t>(pixel[2]) << 16U)
                                   | (static_cast<std::uint32_t>(pixel[3]) << 24U);

        return {
                .r = static_cast<int>(word & 0x3ffU),
                .g = static_cast<int>((word >> 10U) & 0x3ffU),
                .b = static_cast<int>((word >> 20U) & 0x3ffU),
                .a = static_cast<int>(word >> 30U),
        };
    }

private:
    std::vector<std::uint8_t> _bytes;
    bool _hdr;
};

std::unique_ptr<VulkanPainter> &sdr() {
    static std::unique_ptr<VulkanPainter> held = [] {
        std::string error;
        std::unique_ptr<VulkanPainter> made = VulkanPainter::offscreen(SIDE, SIDE, Swapchain::Output::Sdr, &error);

        if (made == nullptr) {
            std::println(stderr, "no painter: {}", error);
        }

        return made;
    }();

    return held;
}

bool near(const Rgba &got, const Rgba &want, const int tolerance) {
    return std::abs(got.r - want.r) <= tolerance && std::abs(got.g - want.g) <= tolerance
           && std::abs(got.b - want.b) <= tolerance;
}

std::string text_of(const Rgba &colour) {
    return std::format("({}, {}, {})", colour.r, colour.g, colour.b);
}

// Checks every pixel and reports the first few that differ, so one bad case does not flood the log.
template <typename Want>
void compare(const Frame &frame, const Rect &area, const int tolerance, Want want, const std::string_view what) {
    int reported = 0;

    for (int y = static_cast<int>(area.y); y < static_cast<int>(area.y + area.height); ++y) {
        for (int x = static_cast<int>(area.x); x < static_cast<int>(area.x + area.width); ++x) {
            const Rgba expected = want(x, y);
            const Rgba got = frame.at(x, y);

            if (!near(got, expected, tolerance) && reported < 4) {
                check(false, std::format("{} at {},{}: got {}, want {}", what, x, y, text_of(got), text_of(expected)));
                ++reported;
            }
        }
    }
}

std::vector<std::uint8_t> noise(const int width, const int height, const unsigned seed) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    std::mt19937 random(seed);

    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels.at(i) = i % 4 == 3 ? 255 : static_cast<std::uint8_t>(random());
    }

    return pixels;
}

Rgba texel(const std::vector<std::uint8_t> &pixels, const int width, const int x, const int y) {
    const std::size_t at =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)) * 4;

    return {.r = pixels.at(at), .g = pixels.at(at + 1), .b = pixels.at(at + 2), .a = pixels.at(at + 3)};
}

Painter::Tile tile_of(Painter &painter, const std::vector<std::uint8_t> &pixels, const int width, const int height) {
    const Painter::Tile tile = painter.acquire_tile(width, height, Bitmap::Encoding::Srgb);

    painter.upload(tile, pixels.data(), static_cast<std::size_t>(width) * 4);

    return tile;
}

Frame draw(VulkanPainter &painter,
           const Colour clear,
           const std::function<void(Painter &)> &body,
           const bool hdr = false) {
    if (!painter.begin_frame(clear)) {
        return {{}, hdr};
    }

    body(painter);
    painter.end_frame();

    return {painter.read(), hdr};
}

void clears() {
    VulkanPainter &painter = *sdr();
    const Frame frame = draw(painter, {.r = 10, .g = 20, .b = 30}, [](Painter &) {});

    compare(
            frame,
            {.width = SIDE, .height = SIDE},
            0,
            [](int, int) { return Rgba{.r = 10, .g = 20, .b = 30}; },
            "clear");
}

void one_to_one() {
    VulkanPainter &painter = *sdr();
    const std::vector<std::uint8_t> pixels = noise(200, 150, 1);
    const Painter::Tile tile = tile_of(painter, pixels, 200, 150);
    const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
        p.tile(tile, {.width = 200, .height = 150}, {.x = 7, .y = 9, .width = 200, .height = 150}, 1);
    });

    compare(
            frame,
            {.x = 7, .y = 9, .width = 200, .height = 150},
            1,
            [&](const int x, const int y) { return texel(pixels, 200, x - 7, y - 9); },
            "1:1");
    painter.release_tile(tile);
}

// The sharp bilinear filter of the tile shader, worked on the CPU for a scale on both axes.
Rgba sharp(const std::vector<std::uint8_t> &pixels,
           const int width,
           const int height,
           const double scale,
           const int x,
           const int y) {
    const double box = std::min(1.0 / scale, 1.0);
    std::array<double, 2> at{};

    for (int axis = 0; axis < 2; ++axis) {
        const double centre = ((axis == 0 ? x : y) + 0.5) / scale;
        const double start = centre - (0.5 * box);
        const double fraction = start - std::floor(start);
        const double t = std::clamp((fraction - (1.0 - box)) / box, 0.0, 1.0);
        const double offset = t * t * (3.0 - (2.0 * t));
        const double limit = axis == 0 ? width : height;

        at.at(static_cast<std::size_t>(axis)) = std::clamp(std::floor(start) + 0.5 + offset, 0.5, limit - 0.5);
    }

    const double fx = at.at(0) - 0.5;
    const double fy = at.at(1) - 0.5;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const double wx = fx - x0;
    const double wy = fy - y0;
    const auto get = [&](const int sx, const int sy) {
        return texel(pixels, width, std::clamp(sx, 0, width - 1), std::clamp(sy, 0, height - 1));
    };
    const auto mix = [&](const auto channel) {
        const double top = (channel(get(x0, y0)) * (1.0 - wx)) + (channel(get(x0 + 1, y0)) * wx);
        const double bottom = (channel(get(x0, y0 + 1)) * (1.0 - wx)) + (channel(get(x0 + 1, y0 + 1)) * wx);

        return static_cast<int>(std::lround((top * (1.0 - wy)) + (bottom * wy)));
    };

    return {
            .r = mix([](const Rgba &c) { return c.r; }),
            .g = mix([](const Rgba &c) { return c.g; }),
            .b = mix([](const Rgba &c) { return c.b; }),
            .a = 255,
    };
}

void filtered() {
    VulkanPainter &painter = *sdr();

    for (const double scale : {2.5, 0.75, 1.3}) {
        const std::vector<std::uint8_t> pixels = noise(64, 64, 2);
        const Painter::Tile tile = tile_of(painter, pixels, 64, 64);
        const double side = 64.0 * scale;
        const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
            p.tile(tile, {.width = 64, .height = 64}, {.width = side, .height = side}, 1);
        });

        compare(
                frame,
                {.width = std::floor(side), .height = std::floor(side)},
                2,
                [&](const int x, const int y) { return sharp(pixels, 64, 64, scale, x, y); },
                std::format("filtered at {}", scale));
        painter.release_tile(tile);
    }
}

void orientations() {
    VulkanPainter &painter = *sdr();
    constexpr int WIDTH = 3;
    constexpr int HEIGHT = 2;
    constexpr double SCALE = 4.0;
    const std::vector<std::uint8_t> pixels = noise(WIDTH, HEIGHT, 3);
    const Painter::Tile tile = tile_of(painter, pixels, WIDTH, HEIGHT);

    for (int orientation = 1; orientation <= 8; ++orientation) {
        const bool swapped = tiv::Orient::swaps(orientation);
        const Rect target{
                .x = 10.0,
                .y = 20.0,
                .width = (swapped ? HEIGHT : WIDTH) * SCALE,
                .height = (swapped ? WIDTH : HEIGHT) * SCALE,
        };
        const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
            p.tile(tile, {.width = WIDTH, .height = HEIGHT}, target, orientation);
        });

        for (int sy = 0; sy < HEIGHT; ++sy) {
            for (int sx = 0; sx < WIDTH; ++sx) {
                const Rect unit = tiv::oriented(
                        {
                                .x = sx / double{WIDTH},
                                .y = sy / double{HEIGHT},
                                .width = 1.0 / WIDTH,
                                .height = 1.0 / HEIGHT,
                        },
                        orientation);
                const Rect lands{
                        .x = target.x + (unit.x * target.width),
                        .y = target.y + (unit.y * target.height),
                        .width = unit.width * target.width,
                        .height = unit.height * target.height,
                };

                compare(
                        frame,
                        lands,
                        1,
                        [&](int, int) { return texel(pixels, WIDTH, sx, sy); },
                        std::format("orientation {}", orientation));
            }
        }
    }

    painter.release_tile(tile);
}

void no_gaps() {
    VulkanPainter &painter = *sdr();
    const std::vector<std::uint8_t> white(std::size_t{100} * 100 * 4, 255);
    std::array<Painter::Tile, 4> tiles{};

    for (Painter::Tile &tile : tiles) {
        tile = tile_of(painter, white, 100, 100);
    }

    constexpr double SCALE = 1.137;
    constexpr double LEFT = 3.3;
    constexpr double TOP = 5.7;
    const Frame frame = draw(painter, {.r = 255}, [&](Painter &p) {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const double x = LEFT + (static_cast<double>(i % 2) * 100.0 * SCALE);
            const double y = TOP + (static_cast<double>(i >> 1U) * 100.0 * SCALE);

            p.tile(tiles.at(i),
                   {.width = 100, .height = 100},
                   {.x = x, .y = y, .width = 100 * SCALE, .height = 100 * SCALE},
                   1);
        }
    });

    compare(
            frame,
            {
                    .x = std::ceil(LEFT),
                    .y = std::ceil(TOP),
                    .width = std::floor(200 * SCALE) - 1,
                    .height = std::floor(200 * SCALE) - 1,
            },
            0,
            [](int, int) { return Rgba{.r = 255, .g = 255, .b = 255}; },
            "seam");

    for (const Painter::Tile tile : tiles) {
        painter.release_tile(tile);
    }
}

void text() {
    VulkanPainter &painter = *sdr();

    for (const float scale : {1.0F, 2.0F}) {
        const Frame frame = draw(
                painter, CLEAR, [&](Painter &p) { p.text(5.0F, 5.0F, scale, "A", {.r = 255, .g = 255, .b = 255}); });
        const std::span<const std::uint8_t, tiv::Font::SIZE> rows = tiv::Font::rows(tiv::Font::glyph(U'A'));
        const auto step = static_cast<int>(scale);

        compare(
                frame,
                {.width = 40, .height = 40},
                0,
                [&](const int x, const int y) {
                    const int gx = (x - 5) / step;
                    const int gy = (y - 5) / step;
                    const bool inside = x >= 5 && y >= 5 && gx < tiv::Font::SIZE && gy < tiv::Font::SIZE;
                    const bool lit =
                            inside && (rows[static_cast<std::size_t>(gy)] & (1U << static_cast<unsigned>(gx))) != 0;

                    return lit ? Rgba{.r = 255, .g = 255, .b = 255} : Rgba{};
                },
                std::format("glyph at {}", scale));
    }
}

// At 1.5 a glyph edge falls halfway across a pixel. SDL 3.4 darkened such an edge as it faded.
// That reads sharper than a lighter blend, and the bars keep the look.
void text_edges() {
    VulkanPainter &painter = *sdr();
    const Frame frame = draw(painter, {.r = 24, .g = 24, .b = 24}, [](Painter &p) {
        p.text(4.0F, 4.0F, 1.5F, "M", {.r = 220, .g = 220, .b = 220});
    });
    int dark = 0;
    int light = 0;

    for (int y = 4; y < 16; ++y) {
        for (int x = 4; x < 16; ++x) {
            const int red = frame.at(x, y).r;

            dark += std::abs(red - 67) <= 1 ? 1 : 0;
            light += std::abs(red - 122) <= 1 ? 1 : 0;
        }
    }

    check(dark > 0 && light == 0, std::format("edges: {} dark, {} light", dark, light));
}

void blends() {
    VulkanPainter &painter = *sdr();
    const Frame frame = draw(painter, {.r = 50, .g = 100, .b = 150}, [](Painter &p) {
        p.fill({.x = 10, .y = 10, .width = 20, .height = 20}, {.r = 24, .g = 24, .b = 24, .a = 216});
    });
    const auto over = [](const int source, const int under) {
        return static_cast<int>(std::lround(((source * 216.0) + (under * 39.0)) / 255.0));
    };

    compare(
            frame,
            {.x = 10, .y = 10, .width = 20, .height = 20},
            1,
            [&](int, int) { return Rgba{.r = over(24, 50), .g = over(24, 100), .b = over(24, 150)}; },
            "badge");
}

void views() {
    VulkanPainter &painter = *sdr();
    const Rect view{.x = 10, .y = 30, .width = 20, .height = 25};
    const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
        p.set_view(&view);
        p.fill({.x = -5, .y = -5, .width = 100, .height = 100}, {.g = 255});
        p.set_view(nullptr);
        p.fill({.x = 100, .y = 100, .width = 2, .height = 2}, {.b = 255});
    });

    compare(
            frame,
            {.width = 120, .height = 120},
            0,
            [&](const int x, const int y) {
                if (x >= 10 && x < 30 && y >= 30 && y < 55) {
                    return Rgba{.g = 255};
                }

                return x >= 100 && x < 102 && y >= 100 && y < 102 ? Rgba{.b = 255} : Rgba{};
            },
            "view");
}

void brightness() {
    VulkanPainter &painter = *sdr();
    const Frame frame = draw(painter, CLEAR, [](Painter &p) {
        p.set_brightness(0.5F);
        p.fill({.width = 4, .height = 4}, {.r = 255, .g = 255, .b = 255});
        p.set_brightness(1.0F);
    });

    compare(
            frame,
            {.width = 4, .height = 4},
            1,
            [](int, int) { return Rgba{.r = 128, .g = 128, .b = 128}; },
            "brightness");
}

// Far more bytes than the staging ring holds, so it wraps and waits several times over.
void ring_wraps() {
    VulkanPainter &painter = *sdr();
    constexpr int COUNT = 72;
    std::vector<Painter::Tile> tiles;
    std::vector<std::uint8_t> pixels(std::size_t{1024} * 1024 * 4);

    for (int i = 0; i < COUNT; ++i) {
        for (std::size_t at = 0; at < pixels.size(); ++at) {
            pixels.at(at) = at % 4 == 3 ? 255 : static_cast<std::uint8_t>(i * 3);
        }

        tiles.push_back(tile_of(painter, pixels, 1024, 1024));
    }

    const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
        for (int i = 0; i < COUNT; ++i) {
            p.tile(tiles.at(static_cast<std::size_t>(i)),
                   {.width = 1, .height = 1},
                   {.x = static_cast<double>(i), .width = 1, .height = 1},
                   1);
        }
    });

    compare(
            frame,
            {.width = COUNT, .height = 1},
            0,
            [](const int x, int) {
                const int value = x * 3;

                return Rgba{.r = value, .g = value, .b = value};
            },
            "ring");

    for (const Painter::Tile tile : tiles) {
        painter.release_tile(tile);
    }
}

// Sixteen tiles of 256 share one layer. None may show a texel of another when filtered.
void cells_stay_apart() {
    VulkanPainter &painter = *sdr();
    constexpr int COUNT = 16;
    constexpr int SIDE_OF = 200;
    constexpr double SCALE = 0.31;
    std::vector<Painter::Tile> tiles;

    for (int i = 0; i < COUNT; ++i) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(SIDE_OF) * SIDE_OF * 4);

        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            pixels.at(at) = static_cast<std::uint8_t>(i * 15);
            pixels.at(at + 1) = static_cast<std::uint8_t>(255 - (i * 15));
            pixels.at(at + 2) = 77;
            pixels.at(at + 3) = 255;
        }

        tiles.push_back(tile_of(painter, pixels, SIDE_OF, SIDE_OF));
    }

    const double side = SIDE_OF * SCALE;
    const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
        for (int i = 0; i < COUNT; ++i) {
            const int row = i / 4;

            p.tile(tiles.at(static_cast<std::size_t>(i)),
                   {.width = SIDE_OF, .height = SIDE_OF},
                   {.x = (i % 4) * 64.0, .y = row * 64.0, .width = side, .height = side},
                   1);
        }
    });

    for (int i = 0; i < COUNT; ++i) {
        const Rgba want{.r = i * 15, .g = 255 - (i * 15), .b = 77};
        const int row = i / 4;

        compare(
                frame,
                {
                        .x = (i % 4) * 64.0,
                        .y = row * 64.0,
                        .width = std::floor(side),
                        .height = std::floor(side),
                },
                0,
                [&](int, int) { return want; },
                std::format("cell {}", i));
    }

    for (const Painter::Tile tile : tiles) {
        painter.release_tile(tile);
    }
}

void reuses_layers() {
    VulkanPainter &painter = *sdr();
    const std::vector<std::uint8_t> red = {255, 0, 0, 255};
    const std::vector<std::uint8_t> blue = {0, 0, 255, 255};
    const Painter::Tile first = tile_of(painter, red, 1, 1);

    static_cast<void>(draw(painter, CLEAR, [&](Painter &p) {
        p.tile(first, {.width = 1, .height = 1}, {.width = 4, .height = 4}, 1);
    }));
    painter.release_tile(first);

    const Painter::Tile second = tile_of(painter, blue, 1, 1);
    const Frame frame = draw(painter, CLEAR, [&](Painter &p) {
        p.tile(second, {.width = 1, .height = 1}, {.width = 4, .height = 4}, 1);
    });

    check(first == second, "a released layer is the next one handed out");
    compare(frame, {.width = 4, .height = 4}, 0, [](int, int) { return Rgba{.b = 255}; }, "rewritten layer");
    painter.release_tile(second);
}

void pq_passes_through() {
    std::string error;
    std::unique_ptr<VulkanPainter> painter = VulkanPainter::offscreen(SIDE, SIDE, Swapchain::Output::Hdr10, &error);

    if (painter == nullptr) {
        skip(error);

        return;
    }

    constexpr int WIDTH = 120;
    constexpr int HEIGHT = 90;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(WIDTH) * HEIGHT * 4);
    std::vector<Rgba> want;
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp): the pixels must repeat from run to run.
    std::mt19937 random(4);

    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const Rgba colour{
                .r = static_cast<int>(random() & 0x3ffU),
                .g = static_cast<int>(random() & 0x3ffU),
                .b = static_cast<int>(random() & 0x3ffU),
                .a = 3,
        };
        const std::uint32_t word = static_cast<std::uint32_t>(colour.r) | (static_cast<std::uint32_t>(colour.g) << 10U)
                                   | (static_cast<std::uint32_t>(colour.b) << 20U) | (3U << 30U);

        for (std::size_t byte = 0; byte < 4; ++byte) {
            pixels.at(i + byte) = static_cast<std::uint8_t>(word >> (8 * byte));
        }

        want.push_back(colour);
    }

    const Painter::Tile tile = painter->acquire_tile(WIDTH, HEIGHT, Bitmap::Encoding::Pq);

    painter->upload(tile, pixels.data(), std::size_t{WIDTH} * 4);

    const Frame frame = draw(
            *painter,
            CLEAR,
            [&](Painter &p) {
                p.tile(tile, {.width = WIDTH, .height = HEIGHT}, {.x = 3, .y = 4, .width = WIDTH, .height = HEIGHT}, 1);
            },
            true);

    compare(
            frame,
            {.x = 3, .y = 4, .width = WIDTH, .height = HEIGHT},
            2,
            [&](const int x, const int y) {
                return want.at((static_cast<std::size_t>(y - 4) * WIDTH) + static_cast<std::size_t>(x - 3));
            },
            "PQ 1:1");
    painter->release_tile(tile);

    // A bar on a PQ output is encoded on the CPU, white at the white of the images.
    painter->set_hdr({.minimum = 0.0F, .peak = 600.0F, .whiteNits = 100.0F});

    const Frame bar = draw(
            *painter,
            CLEAR,
            [](Painter &p) { p.fill({.width = 8, .height = 8}, {.r = 255, .g = 255, .b = 255}); },
            true);
    const auto white = static_cast<int>(std::lround(tiv::Tone::pq_signal(100.0F) * 1023.0F));

    compare(
            bar,
            {.width = 8, .height = 8},
            5,
            [&](int, int) { return Rgba{.r = white, .g = white, .b = white}; },
            "PQ bar");
}

void guarded(void (*body)()) {
    if (sdr() == nullptr) {
        skip("no Vulkan device");

        return;
    }

    body();
}
}

void register_painter() {
    add("painter/clears", [] { guarded(clears); });
    add("painter/one_to_one", [] { guarded(one_to_one); });
    add("painter/filtered", [] { guarded(filtered); });
    add("painter/orientations", [] { guarded(orientations); });
    add("painter/no_gaps", [] { guarded(no_gaps); });
    add("painter/text", [] { guarded(text); });
    add("painter/text_edges", [] { guarded(text_edges); });
    add("painter/blends", [] { guarded(blends); });
    add("painter/views", [] { guarded(views); });
    add("painter/brightness", [] { guarded(brightness); });
    add("painter/ring_wraps", [] { guarded(ring_wraps); });
    add("painter/cells_stay_apart", [] { guarded(cells_stay_apart); });
    add("painter/reuses_layers", [] { guarded(reuses_layers); });
    add("painter/pq_passes_through", pq_passes_through);
    add("painter/validation",
        [] { check(tiv::gpu::Device::validation_messages() == 0, "the validation layers reported nothing"); });
    at_end([] { sdr() = nullptr; });
}
}
