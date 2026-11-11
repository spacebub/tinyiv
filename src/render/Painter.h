// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_PAINTER_H
#define TIV_RENDER_PAINTER_H


#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "image/Bitmap.h"
#include "view/Viewport.h"

namespace tiv {
struct Colour {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct Point {
    float x = 0.0F;
    float y = 0.0F;
};

class Painter {

public:
    using Tile = std::uint32_t;
    static constexpr Tile NO_TILE = 0;

    Painter() = default;
    virtual ~Painter() = default;

    Painter(const Painter &) = delete;
    Painter(Painter &&) = delete;
    Painter &operator=(const Painter &) = delete;
    Painter &operator=(Painter &&) = delete;

    // False for a window shrunk to nothing. No draw call or end_frame() may follow then.
    virtual bool begin_frame(Colour clear) = 0;
    virtual void end_frame() = 0;

    // Later draws are offset by the rect and clipped to it. Null resets.
    virtual void set_view(const Rect *view) = 0;

    virtual void set_brightness(float scale) = 0;

    virtual void tile(Tile tile, const Rect &source, const Rect &target, int orientation) = 0;

    virtual void fill(const Rect &rect, Colour colour) = 0;

    virtual void triangles(std::span<const Point> points, Colour colour) = 0;

    virtual void text(float x, float y, float scale, std::string_view text, Colour colour) = 0;

    // NO_TILE when there is no room.
    [[nodiscard]] virtual Tile acquire_tile(int width, int height, Bitmap::Encoding encoding) = 0;
    virtual void release_tile(Tile tile) = 0;

    virtual void upload(Tile tile, const std::uint8_t *pixels, std::size_t pitch) = 0;

    [[nodiscard]] virtual int max_tile() const = 0;
};
}


#endif //TIV_RENDER_PAINTER_H
