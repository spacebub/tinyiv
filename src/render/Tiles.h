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
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "view/Viewport.h"

namespace tiv {
    // One bitmap on the GPU as a grid of textures, each uploaded when something asks for it.
    class Tiles {

    public:
        // Small enough that a screenful is a handful of uploads, none of which stalls a frame.
        static constexpr int SIZE = 1024;

        // Called for the parts of a drawn area that have no texture yet, in bitmap pixels.
        using Missing = std::function<void(const Rect &)>;

        Tiles(SDL_Renderer *renderer, int maxTexture, std::shared_ptr<const Bitmap> bitmap);
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

        // The area, in bitmap pixels, lands in dest. Tiles drawn are stamped as used.
        void draw(const Rect &area, const Rect &dest, std::uint64_t stamp, const Missing &missing);

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
        [[nodiscard]] Tile &at(int column, int row) { return _tiles.at((static_cast<std::size_t>(row) * static_cast<std::size_t>(_columns)) + static_cast<std::size_t>(column)); }
        [[nodiscard]] const Tile &at(int column, int row) const { return _tiles.at((static_cast<std::size_t>(row) * static_cast<std::size_t>(_columns)) + static_cast<std::size_t>(column)); }
        [[nodiscard]] static std::size_t bytes_of(const Tile &tile);
        void fill(const Tile &tile) const;
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
    };
}


#endif //TIV_RENDER_TILES_H
