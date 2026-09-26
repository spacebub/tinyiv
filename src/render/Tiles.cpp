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
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "render/Tiles.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        Rect intersect(const Rect &a, const Rect &b) {
            const double x0 = std::max(a.x, b.x);
            const double y0 = std::max(a.y, b.y);
            const double x1 = std::min(a.x + a.width, b.x + b.width);
            const double y1 = std::min(a.y + a.height, b.y + b.height);

            if (x1 <= x0 || y1 <= y0) {
                return {};
            }

            return {x0, y0, x1 - x0, y1 - y0};
        }

        Rect of(const SDL_Rect &rect) {
            return {static_cast<double>(rect.x), static_cast<double>(rect.y), static_cast<double>(rect.w), static_cast<double>(rect.h)};
        }
    }

    Tiles::Tiles(SDL_Renderer *renderer, const int maxTexture, std::shared_ptr<const Bitmap> bitmap)
        : _renderer(renderer), _width(bitmap->width()), _height(bitmap->height()), _size(std::min(SIZE, maxTexture)),
          _columns((_width + _size - 1) / _size), _rows((_height + _size - 1) / _size), _bitmap(std::move(bitmap)) {
        _tiles.reserve(static_cast<std::size_t>(_columns) * static_cast<std::size_t>(_rows));

        for (int row = 0; row < _rows; ++row) {
            for (int column = 0; column < _columns; ++column) {
                Tile tile;
                const int x = column * _size;
                const int y = row * _size;

                tile.area = {x, y, std::min(_size, _width - x), std::min(_size, _height - y)};
                _tiles.push_back(tile);
            }
        }
    }

    Tiles::~Tiles() {
        for (Tile &tile : _tiles) {
            release(tile);
        }
    }

    void Tiles::rebind(std::shared_ptr<const Bitmap> bitmap) {
        if (bitmap->width() == _width && bitmap->height() == _height) {
            _bitmap = std::move(bitmap);
        }
    }

    std::size_t Tiles::bytes_of(const Tile &tile) {
        return static_cast<std::size_t>(tile.area.w) * static_cast<std::size_t>(tile.area.h) * Bitmap::CHANNELS;
    }

    void Tiles::release(Tile &tile) {
        if (tile.texture == nullptr) {
            return;
        }

        SDL_DestroyTexture(tile.texture);
        tile.texture = nullptr;
        --_resident;
        _bytes -= bytes_of(tile);
    }

    Tiles::Span Tiles::columns(const Rect &area) const {
        const int first = std::clamp(static_cast<int>(std::floor(area.x / _size)), 0, _columns - 1);
        const int last = std::clamp(static_cast<int>(std::ceil((area.x + area.width) / _size)) - 1, first, _columns - 1);

        return {first, last};
    }

    Tiles::Span Tiles::rows(const Rect &area) const {
        const int first = std::clamp(static_cast<int>(std::floor(area.y / _size)), 0, _rows - 1);
        const int last = std::clamp(static_cast<int>(std::ceil((area.y + area.height) / _size)) - 1, first, _rows - 1);

        return {first, last};
    }

    std::size_t Tiles::upload(const Rect &area, const std::size_t budget, const std::uint64_t stamp) {
        if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
            return 0;
        }

        const Span cols = columns(area);
        const Span rws = rows(area);
        const double centreX = area.x + (area.width / 2.0);
        const double centreY = area.y + (area.height / 2.0);

        struct Want {
            double distance;
            int column;
            int row;
        };

        std::vector<Want> wants;

        for (int row = rws.first; row <= rws.last; ++row) {
            for (int column = cols.first; column <= cols.last; ++column) {
                const Tile &tile = at(column, row);

                if (tile.texture != nullptr) {
                    continue;
                }

                const double dx = (tile.area.x + (tile.area.w / 2.0)) - centreX;
                const double dy = (tile.area.y + (tile.area.h / 2.0)) - centreY;

                wants.push_back({(dx * dx) + (dy * dy), column, row});
            }
        }

        std::ranges::sort(wants, {}, &Want::distance);

        std::size_t spent = 0;

        for (const Want &want : wants) {
            if (spent >= budget) {
                break;
            }

            Tile &tile = at(want.column, want.row);

            tile.texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tile.area.w, tile.area.h);

            if (tile.texture == nullptr) {
                break;
            }

            const std::span<const std::uint8_t> origin = _bitmap->row(tile.area.y).subspan(static_cast<std::size_t>(tile.area.x) * Bitmap::CHANNELS);

            SDL_UpdateTexture(tile.texture, nullptr, origin.data(), static_cast<int>(_bitmap->pitch()));
            SDL_SetTextureScaleMode(tile.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(tile.texture, SDL_BLENDMODE_BLEND);

            tile.used = stamp;
            ++_resident;
            _bytes += bytes_of(tile);
            spent += bytes_of(tile);
        }

        return spent;
    }

    bool Tiles::covered(const Rect &area) const {
        if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
            return true;
        }

        const Span cols = columns(area);
        const Span rws = rows(area);

        for (int row = rws.first; row <= rws.last; ++row) {
            for (int column = cols.first; column <= cols.last; ++column) {
                if (at(column, row).texture == nullptr) {
                    return false;
                }
            }
        }

        return true;
    }

    void Tiles::draw(const Rect &area, const Rect &dest, const std::uint64_t stamp, const Missing &missing) {
        if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
            return;
        }

        const double scaleX = dest.width / area.width;
        const double scaleY = dest.height / area.height;
        const Span cols = columns(area);
        const Span rws = rows(area);

        for (int row = rws.first; row <= rws.last; ++row) {
            for (int column = cols.first; column <= cols.last; ++column) {
                Tile &tile = at(column, row);
                const Rect part = intersect(area, of(tile.area));

                if (part.width <= 0.0 || part.height <= 0.0) {
                    continue;
                }

                if (tile.texture == nullptr) {
                    missing(part);

                    continue;
                }

                const SDL_FRect source{
                        static_cast<float>(part.x - tile.area.x),
                        static_cast<float>(part.y - tile.area.y),
                        static_cast<float>(part.width),
                        static_cast<float>(part.height),
                };

                const SDL_FRect target{
                        static_cast<float>(dest.x + ((part.x - area.x) * scaleX)),
                        static_cast<float>(dest.y + ((part.y - area.y) * scaleY)),
                        static_cast<float>(part.width * scaleX),
                        static_cast<float>(part.height * scaleY),
                };

                SDL_RenderTexture(_renderer, tile.texture, &source, &target);
                tile.used = stamp;
            }
        }
    }

    std::size_t Tiles::evict(const std::uint64_t before, const std::size_t wanted) {
        std::size_t freed = 0;

        for (Tile &tile : _tiles) {
            if (freed >= wanted) {
                break;
            }

            if (tile.texture != nullptr && tile.used < before) {
                freed += bytes_of(tile);
                release(tile);
            }
        }

        return freed;
    }

    std::size_t Tiles::evict_all() {
        const std::size_t freed = _bytes;

        for (Tile &tile : _tiles) {
            release(tile);
        }

        return freed;
    }
}
