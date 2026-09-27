// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_SHRINK_H
#define TIV_IMAGE_SHRINK_H


#include <cstdint>
#include <span>
#include <vector>

#include "image/Bitmap.h"

namespace tiv {
    // Averages blocks of factor by factor pixels as RGBA8 rows arrive, so a shrink costs its
    // output plus one row of sums, whatever the size of the image.
    class BoxShrink {

    public:
        // Rows arrive top first, from row from on, which is a multiple of factor. Height is
        // the whole image's.
        BoxShrink(int width, int height, int factor, Bitmap *target, int from = 0);

        // Rows arrive bottom first, from the last.
        static BoxShrink upward(int width, int height, int factor, Bitmap *target);

        void push(std::span<const std::uint8_t> row);

        // A row of transparent black, which costs nothing to add.
        void skip();

    private:
        void advance();

        Bitmap *_target;
        int _width;
        int _height;
        int _factor;
        int _y;
        int _gathered = 0;
        bool _upward = false;
        std::vector<std::uint32_t> _sums;
        std::vector<std::uint32_t> _counts;
    };
}


#endif //TIV_IMAGE_SHRINK_H
