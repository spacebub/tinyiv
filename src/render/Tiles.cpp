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

#include "image/Bitmap.h"
#include "render/Painter.h"
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

    return {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
}
}

Tiles::Tiles(Painter &painter, std::shared_ptr<const Bitmap> bitmap)
    : _painter(&painter),
      _width(bitmap->width()),
      _height(bitmap->height()),
      _size(std::min(SIZE, painter.max_tile())),
      _columns((_width + _size - 1) / _size),
      _rows((_height + _size - 1) / _size),
      _bitmap(std::move(bitmap)) {
    lay_out();
}

Tiles::Tiles(Painter &painter, std::shared_ptr<const TileCache> tileCache, const int level)
    : _painter(&painter),
      _width(tileCache->levels()[static_cast<std::size_t>(level)].width),
      _height(tileCache->levels()[static_cast<std::size_t>(level)].height),
      _size(TileCache::TILE),
      _columns((_width + _size - 1) / _size),
      _rows((_height + _size - 1) / _size),
      _tileCache(std::move(tileCache)),
      _level(level) {
    lay_out();
}

void Tiles::lay_out() {
    _tiles.reserve(static_cast<std::size_t>(_columns) * static_cast<std::size_t>(_rows));

    for (int row = 0; row < _rows; ++row) {
        for (int column = 0; column < _columns; ++column) {
            Tile tile;
            const int x = column * _size;
            const int y = row * _size;

            tile.area = {.x = x, .y = y, .w = std::min(_size, _width - x), .h = std::min(_size, _height - y)};
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
        if (tile.handle != Painter::NO_TILE) {
            fill(tile);
        }
    }
}

void Tiles::fill(const Tile &tile) const {
    const std::span<const std::uint8_t> origin =
            _bitmap->row(tile.area.y).subspan(static_cast<std::size_t>(tile.area.x) * Bitmap::CHANNELS);

    _painter->upload(tile.handle, origin.data(), _bitmap->pitch());
}

std::size_t Tiles::bytes_of(const Tile &tile) {
    return static_cast<std::size_t>(tile.area.w) * static_cast<std::size_t>(tile.area.h) * Bitmap::CHANNELS;
}

void Tiles::release(Tile &tile) {
    if (tile.handle == Painter::NO_TILE) {
        return;
    }

    _painter->release_tile(tile.handle);
    tile.handle = Painter::NO_TILE;
    --_resident;
    _bytes -= bytes_of(tile);
}

Tiles::Span Tiles::columns(const Rect &area) const {
    const int first = std::clamp(static_cast<int>(std::floor(area.x / _size)), 0, _columns - 1);
    const int last = std::clamp(static_cast<int>(std::ceil((area.x + area.width) / _size)) - 1, first, _columns - 1);

    return {.first = first, .last = last};
}

Tiles::Span Tiles::rows(const Rect &area) const {
    const int first = std::clamp(static_cast<int>(std::floor(area.y / _size)), 0, _rows - 1);
    const int last = std::clamp(static_cast<int>(std::ceil((area.y + area.height) / _size)) - 1, first, _rows - 1);

    return {.first = first, .last = last};
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

            if (tile.handle != Painter::NO_TILE) {
                continue;
            }

            const double dx = (tile.area.x + (tile.area.w / 2.0)) - centreX;
            const double dy = (tile.area.y + (tile.area.h / 2.0)) - centreY;

            wants.push_back({.distance = (dx * dx) + (dy * dy), .column = column, .row = row});
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

        // A tile on disk waits until the tile cache has read it.
        if (_tileCache != nullptr) {
            pixels = _tileCache->find({.level = _level, .column = column, .row = row});

            if (pixels == nullptr) {
                continue;
            }
        }

        tile.handle = _painter->acquire_tile(
                tile.area.w, tile.area.h, _tileCache != nullptr ? _tileCache->encoding() : _bitmap->encoding());

        if (tile.handle == Painter::NO_TILE) {
            break;
        }

        if (pixels != nullptr) {
            _painter->upload(tile.handle, pixels->data(), pixels->pitch());
            _tileCache->uploaded({.level = _level, .column = column, .row = row});
        } else {
            fill(tile);
        }

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
            if (at(column, row).handle == Painter::NO_TILE) {
                return false;
            }
        }
    }

    return true;
}

bool Tiles::uploadable(const Rect &area) const {
    if (_tileCache == nullptr) {
        return !covered(area);
    }

    return std::ranges::any_of(wanting(area), [&](const std::pair<int, int> &tile) {
        return _tileCache->find({.level = _level, .column = tile.first, .row = tile.second}) != nullptr;
    });
}

std::size_t Tiles::uploadable_bytes(const Rect &area) const {
    std::size_t bytes = 0;

    for (const auto &[column, row] : wanting(area)) {
        if (_tileCache == nullptr || _tileCache->find({.level = _level, .column = column, .row = row}) != nullptr) {
            bytes += bytes_of(at(column, row));
        }
    }

    return bytes;
}

void Tiles::missing(const Rect &area, std::vector<TileCache::Key> *out) const {
    if (_tileCache == nullptr) {
        return;
    }

    for (const auto &[column, row] : wanting(area)) {
        out->push_back({.level = _level, .column = column, .row = row});
    }
}

void Tiles::draw(
        const Rect &area, const Rect &shown, const int orientation, const std::uint64_t stamp, const Missing &missing) {
    if (_tiles.empty() || area.width <= 0.0 || area.height <= 0.0) {
        return;
    }

    const Span cols = columns(area);
    const Span rws = rows(area);

    for (int row = rws.first; row <= rws.last; ++row) {
        for (int column = cols.first; column <= cols.last; ++column) {
            Tile &tile = at(column, row);
            const Rect part = intersect(area,
                                        {
                                                .x = static_cast<double>(tile.area.x),
                                                .y = static_cast<double>(tile.area.y),
                                                .width = static_cast<double>(tile.area.w),
                                                .height = static_cast<double>(tile.area.h),
                                        });

            if (part.width <= 0.0 || part.height <= 0.0) {
                continue;
            }

            const Rect unit = oriented(
                    {
                            .x = part.x / _width,
                            .y = part.y / _height,
                            .width = part.width / _width,
                            .height = part.height / _height,
                    },
                    orientation);
            const Rect lands{
                    .x = shown.x + (unit.x * shown.width),
                    .y = shown.y + (unit.y * shown.height),
                    .width = unit.width * shown.width,
                    .height = unit.height * shown.height,
            };

            if (tile.handle == Painter::NO_TILE) {
                missing(lands);

                continue;
            }

            const Rect source{
                    .x = part.x - tile.area.x,
                    .y = part.y - tile.area.y,
                    .width = part.width,
                    .height = part.height,
            };

            _painter->tile(tile.handle, source, lands, orientation);
            tile.used = stamp;
        }
    }
}

std::size_t Tiles::evict(const std::uint64_t before, const std::size_t wanted) {
    std::vector<Tile *> stale;

    for (Tile &tile : _tiles) {
        if (tile.handle != Painter::NO_TILE && tile.used < before) {
            stale.push_back(&tile);
        }
    }

    std::ranges::stable_sort(stale, std::ranges::less(), [](const Tile *tile) { return tile->used; });

    std::size_t freed = 0;

    for (Tile *tile : stale) {
        if (freed >= wanted) {
            break;
        }

        freed += bytes_of(*tile);
        release(*tile);
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
