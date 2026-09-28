// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_TILES_H
#define TIV_RENDER_TILES_H


#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "image/TileCache.h"
#include "view/Viewport.h"

namespace tiv {
    // One bitmap on the GPU as a grid of textures, each uploaded when something asks for it.
    class Tiles {

    public:
        // Small enough that a screenful is a handful of uploads, none of which stalls a frame.
        static constexpr int SIZE = 1024;

        // Called for the parts of a drawn area that have no texture yet, with where they land on screen.
        using Missing = std::function<void(const Rect &)>;

        Tiles(SDL_Renderer *renderer, int maxTexture, std::shared_ptr<const Bitmap> bitmap);

        // A level of a pyramid on disk. Tiles upload once the tile cache has them in memory.
        Tiles(SDL_Renderer *renderer, std::shared_ptr<const TileCache> tileCache, int level);
        ~Tiles();

        Tiles(const Tiles &) = delete;
        Tiles(Tiles &&) = delete;
        Tiles &operator=(const Tiles &) = delete;
        Tiles &operator=(Tiles &&) = delete;

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }
        [[nodiscard]] bool complete() const { return _resident == _tiles.size(); }
        [[nodiscard]] std::size_t resident_bytes() const { return _bytes; }

        // A bitmap of the same size whose pixels later uploads read.
        void rebind(std::shared_ptr<const Bitmap> bitmap);

        // Like rebind(), and every resident tile is rewritten from the new pixels.
        void refresh(std::shared_ptr<const Bitmap> bitmap);

        // Uploads the tiles meeting the area, nearest its centre first, until the budget is
        // spent. Returns the bytes spent.
        std::size_t upload(const Rect &area, std::size_t budget, std::uint64_t stamp);

        [[nodiscard]] bool covered(const Rect &area) const;

        // True when a tile meeting the area has no texture and its pixels are at hand, so
        // upload() would do something.
        [[nodiscard]] bool uploadable(const Rect &area) const;

        // The tile cache's keys of the tiles meeting the area that have no texture, nearest its
        // centre first. Nothing for a bitmap, which has every tile at hand.
        void missing(const Rect &area, std::vector<TileCache::Key> *out) const;

        // The area, in bitmap pixels, of the bitmap shown whole in the screen rect with the
        // orientation. Tiles drawn are stamped as used.
        void draw(const Rect &area, const Rect &shown, int orientation, std::uint64_t stamp, const Missing &missing);

        // Frees tiles last used before the stamp until the wanted bytes are freed. Returns what was freed.
        std::size_t evict(std::uint64_t before, std::size_t wanted);

        std::size_t evict_all();

    private:
        struct Tile {
            SDL_Texture *texture = nullptr;
            // What the tile shows, in bitmap pixels.
            SDL_Rect area{};
            std::uint64_t used = 0;
        };

        struct Span {
            int first = 0;
            int last = 0;
        };

        [[nodiscard]] Span columns(const Rect &area) const;
        [[nodiscard]] Span rows(const Rect &area) const;
        [[nodiscard]] Tile &at(int column, int row) {
            return _tiles.at((static_cast<std::size_t>(row) * static_cast<std::size_t>(_columns))
                             + static_cast<std::size_t>(column));
        }
        [[nodiscard]] const Tile &at(int column, int row) const {
            return _tiles.at((static_cast<std::size_t>(row) * static_cast<std::size_t>(_columns))
                             + static_cast<std::size_t>(column));
        }
        void lay_out();
        [[nodiscard]] static std::size_t bytes_of(const Tile &tile);
        void fill(const Tile &tile) const;
        // The tiles meeting the area without a texture, nearest its centre first.
        [[nodiscard]] std::vector<std::pair<int, int>> wanting(const Rect &area) const;
        void release(Tile &tile);

        SDL_Renderer *_renderer;
        int _width;
        int _height;
        int _size;
        int _columns;
        int _rows;
        std::vector<Tile> _tiles;
        std::size_t _resident = 0;
        std::size_t _bytes = 0;
        std::shared_ptr<const Bitmap> _bitmap;
        // Instead of the bitmap, for a level on disk.
        std::shared_ptr<const TileCache> _tileCache;
        int _level = 0;
    };
}


#endif //TIV_RENDER_TILES_H
