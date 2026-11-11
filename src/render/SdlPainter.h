// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_SDLPAINTER_H
#define TIV_RENDER_SDLPAINTER_H


#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "render/Painter.h"

namespace tiv {
class SdlPainter final : public Painter {

public:
    explicit SdlPainter(SDL_Renderer *renderer);
    ~SdlPainter() override;

    SdlPainter(const SdlPainter &) = delete;
    SdlPainter(SdlPainter &&) = delete;
    SdlPainter &operator=(const SdlPainter &) = delete;
    SdlPainter &operator=(SdlPainter &&) = delete;

    [[nodiscard]] SDL_Renderer *renderer() const { return _renderer; }

    bool begin_frame(Colour clear) override;
    void end_frame() override;
    void set_view(const Rect *view) override;
    void set_brightness(float scale) override;
    void tile(Tile tile, const Rect &source, const Rect &target, int orientation) override;
    void fill(const Rect &rect, Colour colour) override;
    void triangles(std::span<const Point> points, Colour colour) override;
    void text(float x, float y, float scale, std::string_view text, Colour colour) override;
    [[nodiscard]] Tile acquire_tile(int width, int height, Bitmap::Encoding encoding) override;
    void release_tile(Tile tile) override;
    void upload(Tile tile, const std::uint8_t *pixels, std::size_t pitch) override;
    [[nodiscard]] int max_tile() const override;

private:
    [[nodiscard]] SDL_Texture *texture(Tile tile) const;

    SDL_Renderer *_renderer;
    std::vector<SDL_Texture *> _textures;
    std::vector<Tile> _free;
};
}


#endif //TIV_RENDER_SDLPAINTER_H
