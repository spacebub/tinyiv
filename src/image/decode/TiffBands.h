// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_TIFFBANDS_H
#define TIV_DECODE_TIFFBANDS_H


#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

// NOLINTNEXTLINE(readability-identifier-naming): libtiff's own name for its handle.
struct tiff;

namespace tiv::Decode {
    // A TIFF read by its strips or tiles, each of which decodes on its own, so any band of rows decodes
    // without the rest and bands decode side by side. libtiff does the reading, a handle per thread
    // since one handle cannot be shared: https://libtiff.gitlab.io/libtiff/
    // vliv shows strips as tiles the same way: https://github.com/delhoume/vliv (MIT)
    class TiffBands {

    public:
        // Null unless the first image is 8 bit unsigned grey or RGB, with or without an unassociated
        // alpha, its samples interleaved. Anything else goes through libvips.
        [[nodiscard]] static std::unique_ptr<TiffBands> open(const std::filesystem::path &file);

        ~TiffBands();

        TiffBands(const TiffBands &) = delete;
        TiffBands(TiffBands &&) = delete;
        TiffBands &operator=(const TiffBands &) = delete;
        TiffBands &operator=(TiffBands &&) = delete;

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }
        [[nodiscard]] bool alpha() const { return _samples == 2 || _samples == 4; }

        // The rows from top, count of them or to the bottom, as RGBA into out, which is as wide as
        // the image. Uses up to threads threads.
        bool decode(int top, int count, Bitmap *out, int threads, const Abort *abort) const;

    private:
        struct Unit {
            int x = 0;
            int y = 0;
            std::uint32_t index = 0;
        };

        TiffBands() = default;

        // A handle for one thread, back in the pool once it is done.
        [[nodiscard]] tiff *take() const;
        void give(tiff *handle) const;

        // The strips or tiles meeting the rows.
        [[nodiscard]] std::vector<Unit> units(int top, int bottom) const;
        bool read(tiff *handle, const Unit &unit, int top, int bottom, std::vector<std::uint8_t> &scratch,
                  Bitmap *out) const;

        std::filesystem::path _file;
        int _width = 0;
        int _height = 0;
        int _samples = 3;
        bool _tiled = false;
        // A strip's rows, or a tile's size.
        int _unitWidth = 0;
        int _unitHeight = 0;
        std::size_t _unitBytes = 0;

        mutable std::mutex _guard;
        mutable std::vector<tiff *> _idle;
    };
}


#endif //TIV_DECODE_TIFFBANDS_H
