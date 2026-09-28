// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_CANVAS_H
#define TIV_RENDER_CANVAS_H


#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Pyramid.h"
#include "image/TileCache.h"
#include "render/Tiles.h"
#include "view/Viewport.h"

namespace tiv {
    // The image on screen as tiles of every level, only what the view meets on the GPU, and
    // the neighbours' screen sized levels kept ready so a step needs no upload at all.
    class Canvas {

    public:
        static constexpr std::size_t DEFAULT_VRAM_BUDGET = std::size_t{1024} * 1024 * 1024;

        Canvas(SDL_Renderer *renderer, int maxTexture, std::size_t vramBudget = DEFAULT_VRAM_BUDGET);

        // The image to draw. Width and height are the image's own as stored, which the pyramid
        // may be smaller than, and the orientation is how it is shown. Levels the same size as
        // before, of the same image, keep their tiles.
        void show(std::shared_ptr<const Pyramid> pyramid, std::uint64_t image, int width, int height, int orientation);

        // The image from a pyramid on disk. Its tiles come as the tile cache reads them, the
        // coarser levels standing in until they do.
        void show(std::shared_ptr<const TileCache> tileCache, std::uint64_t image, int width, int height,
                  int orientation);

        // Asks the tile cache of the image shown for the tiles the view needs, the level below
        // first, since that one fills the screen soonest. Nothing for an image in memory.
        void fetch(const Viewport &viewport);

        // Shows the image drawn another way round. The tiles stay as they are.
        void orient(int orientation);

        // Another frame of the image shown. Levels the same size as the shown ones keep their
        // textures, rewritten in place.
        void replace(std::shared_ptr<const Pyramid> pyramid);

        // A neighbour to have ready: its levels up to the screen stay resident.
        void warm(std::shared_ptr<const Pyramid> pyramid, std::uint64_t image, int width, int height, int orientation);

        // A sharper rendering of part of the image shown, at the scale given, x and y being
        // where the bitmap sits in the image as stored at that scale. It is drawn wherever it is nearer
        // the zoom than the levels are, and goes when another image is shown.
        void refine(std::shared_ptr<const Bitmap> bitmap, double scale, int x, int y);

        // True when the rendering is at this zoom and covers the area, in pixels of the image as stored.
        [[nodiscard]] bool refined(double zoom, const Rect &area) const;

        // Forgets every image but the one shown and the ones listed.
        void keep(std::span<const std::uint64_t> images);

        void clear();

        [[nodiscard]] bool has_image() const { return !_current.sheets.empty(); }

        // Visible tiles missing that could upload now, or warm ones still to come. A tile on
        // disk the tile cache is still reading waits for it, and the tile cache says when it has come.
        [[nodiscard]] bool pending(const Viewport &viewport) const;

        // Visible tiles first, then a margin around them, then the level below, then the
        // neighbours. Stops once the budget in bytes is spent.
        void upload(const Viewport &viewport, std::size_t budget);

        void draw(const Viewport &viewport);

        [[nodiscard]] std::size_t vram() const;

    private:
        struct Sheet {
            // Level width over image width.
            double scale = 1.0;
            std::unique_ptr<Tiles> tiles;
        };

        struct Detail {
            std::uint64_t image = 0;
            double scale = 1.0;
            // In pixels of the image as stored.
            Rect area;
            std::unique_ptr<Tiles> tiles;
        };

        struct Held {
            std::uint64_t image = 0;
            // As stored.
            int width = 0;
            int height = 0;
            int orientation = 1;
            std::shared_ptr<const Pyramid> pyramid;
            // Instead of the pyramid, for an image on disk.
            std::shared_ptr<const TileCache> tileCache;
            // Ascending by scale, so the coarsest first.
            std::vector<Sheet> sheets;
        };

        // Sheets for the pyramid, reusing those of the previous of the same size.
        [[nodiscard]] Held build(std::shared_ptr<const Pyramid> pyramid, std::uint64_t image, int width, int height,
                                 int orientation, Held previous) const;
        [[nodiscard]] Held build(std::shared_ptr<const TileCache> tileCache, std::uint64_t image, int width, int height,
                                 int orientation, Held previous) const;

        // What was shown, to reuse for the image if it is the same one or a warm neighbour.
        // Anything else goes warm, but for an image on disk, which is never kept warm.
        [[nodiscard]] Held take_previous(std::uint64_t image);

        // The coarsest level is what every missing tile falls back to, so it is never missing.
        void settle();

        // The smallest sheet with a texel per screen pixel, else the finest there is.
        [[nodiscard]] static std::size_t wanted(const Held &held, double zoom);

        // The sheet's pixels under the screen rect.
        [[nodiscard]] static Rect area_of(const Held &held, std::size_t sheet, const Rect &screen, const Rect &image);

        // What the whole screen would hold of the image at fit, in bytes of the fit sheet.
        [[nodiscard]] std::size_t fit_sheet(const Held &held) const;

        void draw_sheet(Held &held, std::size_t index, const Rect &screen, const Rect &image);
        [[nodiscard]] bool detail_wins(double zoom, std::size_t sheet) const;
        std::size_t upload_warm(std::size_t budget);
        void evict();

        SDL_Renderer *_renderer;
        int _maxTexture;
        std::size_t _budget;
        Held _current;
        std::vector<Held> _warm;
        Detail _detail;
        std::uint64_t _frame = 1;
        double _areaWidth = 0.0;
        double _areaHeight = 0.0;
    };
}


#endif //TIV_RENDER_CANVAS_H
