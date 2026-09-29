// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_HEIFBANDS_H
#define TIV_DECODE_HEIFBANDS_H


#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

namespace tiv::Decode {
    // A HEIF or AVIF image stored as a grid of tiles, each coded on its own, so any band of rows
    // decodes from the tiles it meets and tiles decode side by side. libheif decodes each tile:
    // https://github.com/strukturag/libheif (LGPL-3.0), a context per thread, since tiles of one
    // are not decoded at once from several.
    class HeifBands {

    public:
        // Null unless the primary image is a grid of more than one tile, 8 bits a sample, SDR,
        // without alpha, and neither turned, mirrored nor cropped.
        [[nodiscard]] static std::unique_ptr<HeifBands> open(const std::filesystem::path &file);

        ~HeifBands();

        HeifBands(const HeifBands &) = delete;
        HeifBands(HeifBands &&) = delete;
        HeifBands &operator=(const HeifBands &) = delete;
        HeifBands &operator=(HeifBands &&) = delete;

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }

        // The rows from top, count of them or to the bottom, as RGBA into out, which is as wide as
        // the image. Uses up to threads threads.
        bool decode(int top, int count, Bitmap *out, int threads, const Abort *abort) const;

    private:
        // A file read into libheif, and its primary image.
        struct Opened;

        HeifBands() = default;

        [[nodiscard]] std::unique_ptr<Opened> take() const;
        void give(std::unique_ptr<Opened> opened) const;
        bool read(const Opened &opened, int column, int row, int top, int bottom, Bitmap *out) const;

        std::filesystem::path _file;
        int _width = 0;
        int _height = 0;
        int _columns = 0;
        int _rows = 0;
        int _tileWidth = 0;
        int _tileHeight = 0;

        mutable std::mutex _guard;
        mutable std::vector<std::unique_ptr<Opened>> _idle;
    };
}


#endif //TIV_DECODE_HEIFBANDS_H
