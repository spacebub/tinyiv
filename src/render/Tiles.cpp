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
#include "image/Orient.h"
#include "render/Tiles.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        // PQ tiles are BT.2020 in PQ, whose 203 nits the renderer puts at SDR white: https://www.itu.int/pub/R-REP-BT.2408
        SDL_Texture *create_texture(SDL_Renderer *renderer, const Bitmap::Encoding encoding, const int width, const int height) {
            if (encoding == Bitmap::Encoding::Srgb) {
                return SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
            }

            constexpr float SDR_WHITE_NITS = 203.0F;
            const SDL_PropertiesID properties = SDL_CreateProperties();

            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR2101010);
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_HDR10);
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
            SDL_SetFloatProperty(properties, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, SDR_WHITE_NITS);

            SDL_Texture *texture = SDL_CreateTextureWithProperties(renderer, properties);

            SDL_DestroyProperties(properties);

            return texture;
        }

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
        lay_out();
    }

    Tiles::Tiles(SDL_Renderer *renderer, std::shared_ptr<const Store> store, const int level)
        : _renderer(renderer), _width(store->levels()[static_cast<std::size_t>(level)].width), _height(store->levels()[static_cast<std::size_t>(level)].height),
          _size(Store::TILE), _columns((_width + _size - 1) / _size), _rows((_height + _size - 1) / _size), _store(std::move(store)), _level(level) {
        lay_out();
    }

    void Tiles::lay_out() {
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

    void Tiles::refresh(std::shared_ptr<const Bitmap> bitmap) {
        if (bitmap->width() != _width || bitmap->height() != _height) {
            return;
        }

        _bitmap = std::move(bitmap);

        for (const Tile &tile : _tiles) {
            if (tile.texture != nullptr) {
                fill(tile);
            }
        }
    }

    void Tiles::fill(const Tile &tile) const {
        const std::span<const std::uint8_t> origin = _bitmap->row(tile.area.y).subspan(static_cast<std::size_t>(tile.area.x) * Bitmap::CHANNELS);

        SDL_UpdateTexture(tile.texture, nullptr, origin.data(), static_cast<int>(_bitmap->pitch()));
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

    std::vector<std::pair<int, int>> Tiles::wanting(const Rect &area) const {
        std::vector<std::pair<int, int>> held;

        if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
            return held;
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
        held.reserve(wants.size());

        for (const Want &want : wants) {
            held.emplace_back(want.column, want.row);
        }

        return held;
    }

    std::size_t Tiles::upload(const Rect &area, const std::size_t budget, const std::uint64_t stamp) {
        std::size_t spent = 0;

        for (const auto &[column, row] : wanting(area)) {
            if (spent >= budget) {
                break;
            }

            Tile &tile = at(column, row);
            std::shared_ptr<const Bitmap> pixels;

            // A tile on disk waits until the store has read it.
            if (_store != nullptr) {
                pixels = _store->find({_level, column, row});

                if (pixels == nullptr) {
                    continue;
                }
            }

            tile.texture = create_texture(_renderer, _store != nullptr ? _store->encoding() : _bitmap->encoding(), tile.area.w, tile.area.h);

            if (tile.texture == nullptr) {
                break;
            }

            if (pixels != nullptr) {
                SDL_UpdateTexture(tile.texture, nullptr, pixels->data(), static_cast<int>(pixels->pitch()));
            } else {
                fill(tile);
            }

            SDL_SetTextureScaleMode(tile.texture, SDL_SCALEMODE_PIXELART);
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

    bool Tiles::uploadable(const Rect &area) const {
        if (_store == nullptr) {
            return !covered(area);
        }

        return std::ranges::any_of(wanting(area), [&](const std::pair<int, int> &tile) { return _store->find({_level, tile.first, tile.second}) != nullptr; });
    }

    void Tiles::missing(const Rect &area, std::vector<Store::Key> *out) const {
        if (_store == nullptr) {
            return;
        }

        for (const auto &[column, row] : wanting(area)) {
            out->push_back({_level, column, row});
        }
    }

    void Tiles::draw(const Rect &area, const Rect &shown, const int orientation, const std::uint64_t stamp, const Missing &missing) {
        if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
            return;
        }

        const Span cols = columns(area);
        const Span rws = rows(area);
        const int quarters = Orient::quarters(orientation);
        const SDL_FlipMode flip = Orient::mirrors(orientation) ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;

        for (int row = rws.first; row <= rws.last; ++row) {
            for (int column = cols.first; column <= cols.last; ++column) {
                Tile &tile = at(column, row);
                const Rect part = intersect(area, of(tile.area));

                if (part.width <= 0.0 || part.height <= 0.0) {
                    continue;
                }

                const Rect unit = oriented({part.x / _width, part.y / _height, part.width / _width, part.height / _height}, orientation);
                const Rect lands{shown.x + (unit.x * shown.width), shown.y + (unit.y * shown.height), unit.width * shown.width, unit.height * shown.height};

                if (tile.texture == nullptr) {
                    missing(lands);

                    continue;
                }

                const SDL_FRect source{
                        static_cast<float>(part.x - tile.area.x),
                        static_cast<float>(part.y - tile.area.y),
                        static_cast<float>(part.width),
                        static_cast<float>(part.height),
                };

                // SDL turns the target about its centre, so a quarter turn starts from the rect on its side.
                const bool across = quarters % 2 == 1;
                const double width = across ? lands.height : lands.width;
                const double height = across ? lands.width : lands.height;
                const SDL_FRect target{
                        static_cast<float>(lands.x + ((lands.width - width) / 2.0)),
                        static_cast<float>(lands.y + ((lands.height - height) / 2.0)),
                        static_cast<float>(width),
                        static_cast<float>(height),
                };

                SDL_RenderTextureRotated(_renderer, tile.texture, &source, &target, 90.0 * quarters, nullptr, flip);
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
