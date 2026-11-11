// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "image/Orient.h"
#include "image/Tone.h"
#include "render/Painter.h"
#include "render/SdlPainter.h"

namespace tiv {
namespace {
// PQ tiles are BT.2020 in PQ, whose 203 nits the renderer puts at SDR white:
// https://www.itu.int/pub/R-REP-BT.2408
SDL_Texture *
create_texture(SDL_Renderer *renderer, const Bitmap::Encoding encoding, const int width, const int height) {
    if (encoding == Bitmap::Encoding::Srgb) {
        return SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    }

    const SDL_PropertiesID properties = SDL_CreateProperties();

    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR2101010);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_HDR10);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetFloatProperty(properties, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, Tone::PQ_REFERENCE_NITS);

    SDL_Texture *texture = SDL_CreateTextureWithProperties(renderer, properties);

    SDL_DestroyProperties(properties);

    return texture;
}

SDL_FRect frect(const Rect &rect) {
    return {
            .x = static_cast<float>(rect.x),
            .y = static_cast<float>(rect.y),
            .w = static_cast<float>(rect.width),
            .h = static_cast<float>(rect.height),
    };
}

void colour_of(SDL_Renderer *renderer, const Colour colour) {
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
}
}

SdlPainter::SdlPainter(SDL_Renderer *renderer) : _renderer(renderer) {
}

SdlPainter::~SdlPainter() {
    for (SDL_Texture *texture : _textures) {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
    }
}

SDL_Texture *SdlPainter::texture(const Tile tile) const {
    return tile == NO_TILE ? nullptr : _textures.at(tile - 1);
}

bool SdlPainter::begin_frame(const Colour clear) {
    SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_NONE);
    colour_of(_renderer, clear);
    SDL_RenderClear(_renderer);

    return true;
}

void SdlPainter::end_frame() {
    SDL_RenderPresent(_renderer);
}

void SdlPainter::set_view(const Rect *view) {
    if (view == nullptr) {
        SDL_SetRenderViewport(_renderer, nullptr);

        return;
    }

    const SDL_Rect rect{
            .x = static_cast<int>(std::lround(view->x)),
            .y = static_cast<int>(std::lround(view->y)),
            .w = static_cast<int>(std::lround(view->width)),
            .h = static_cast<int>(std::lround(view->height)),
    };

    SDL_SetRenderViewport(_renderer, &rect);
}

void SdlPainter::set_brightness(const float scale) {
    SDL_SetRenderColorScale(_renderer, scale);
}

void SdlPainter::tile(const Tile tile, const Rect &source, const Rect &target, const int orientation) {
    SDL_Texture *held = texture(tile);

    if (held == nullptr) {
        return;
    }

    const int quarters = Orient::quarters(orientation);
    const SDL_FlipMode flip = Orient::mirrors(orientation) ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
    const SDL_FRect from = frect(source);
    // SDL turns the target about its centre, so a quarter turn starts from the rect on its side.
    const bool across = quarters % 2 == 1;
    const double width = across ? target.height : target.width;
    const double height = across ? target.width : target.height;
    const SDL_FRect to{
            .x = static_cast<float>(target.x + ((target.width - width) / 2.0)),
            .y = static_cast<float>(target.y + ((target.height - height) / 2.0)),
            .w = static_cast<float>(width),
            .h = static_cast<float>(height),
    };

    SDL_RenderTextureRotated(_renderer, held, &from, &to, 90.0 * quarters, nullptr, flip);
}

void SdlPainter::fill(const Rect &rect, const Colour colour) {
    const SDL_FRect area = frect(rect);

    SDL_SetRenderDrawBlendMode(_renderer, colour.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    colour_of(_renderer, colour);
    SDL_RenderFillRect(_renderer, &area);
}

void SdlPainter::triangles(const std::span<const Point> points, const Colour colour) {
    const SDL_FColor shade{
            .r = static_cast<float>(colour.r) / 255.0F,
            .g = static_cast<float>(colour.g) / 255.0F,
            .b = static_cast<float>(colour.b) / 255.0F,
            .a = static_cast<float>(colour.a) / 255.0F,
    };
    std::vector<SDL_Vertex> vertices;

    SDL_SetRenderDrawBlendMode(_renderer, colour.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    vertices.reserve(points.size());

    for (const Point &point : points) {
        vertices.push_back({.position = {.x = point.x, .y = point.y}, .color = shade, .tex_coord = {}});
    }

    SDL_RenderGeometry(_renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), nullptr, 0);
}

void SdlPainter::text(
        const float x, const float y, const float scale, const std::string_view text, const Colour colour) {
    const std::string held(text);

    SDL_SetRenderScale(_renderer, scale, scale);
    colour_of(_renderer, colour);
    SDL_RenderDebugText(_renderer, x / scale, y / scale, held.c_str());
    SDL_SetRenderScale(_renderer, 1.0F, 1.0F);
}

Painter::Tile SdlPainter::acquire_tile(const int width, const int height, const Bitmap::Encoding encoding) {
    SDL_Texture *made = create_texture(_renderer, encoding, width, height);

    if (made == nullptr) {
        return NO_TILE;
    }

    SDL_SetTextureScaleMode(made, SDL_SCALEMODE_PIXELART);
    SDL_SetTextureBlendMode(made, SDL_BLENDMODE_BLEND);

    if (!_free.empty()) {
        const Tile tile = _free.back();

        _free.pop_back();
        _textures.at(tile - 1) = made;

        return tile;
    }

    _textures.push_back(made);

    return static_cast<Tile>(_textures.size());
}

void SdlPainter::release_tile(const Tile tile) {
    SDL_Texture *held = texture(tile);

    if (held == nullptr) {
        return;
    }

    SDL_DestroyTexture(held);
    _textures.at(tile - 1) = nullptr;
    _free.push_back(tile);
}

void SdlPainter::upload(const Tile tile, const std::uint8_t *pixels, const std::size_t pitch) {
    SDL_UpdateTexture(texture(tile), nullptr, pixels, static_cast<int>(pitch));
}

int SdlPainter::max_tile() const {
    constexpr int MOST = 1024;
    const auto most = static_cast<int>(SDL_GetNumberProperty(
            SDL_GetRendererProperties(_renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, MOST));

    return std::min(MOST, most);
}
}
