// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_PYRAMID_H
#define TIV_IMAGE_PYRAMID_H


#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "image/Bitmap.h"

namespace tiv {
    // One image at every power of two from the decode down to a thumbnail, largest first.
    // Levels are shared, so a copy of a Pyramid is a copy of a few pointers.
    struct Pyramid {
        enum class Kernel : std::uint8_t {
            Auto,
            Scalar,
            Sse2,
            Avx2,
        };

        std::vector<std::shared_ptr<const Bitmap>> levels;

        // Base becomes the first level, and halving stops at the first level within the box.
        static Pyramid build(Bitmap base, int thumbWidth, int thumbHeight);

        // Averages 2x2 blocks. An odd last row or column is averaged with itself.
        static Bitmap halve(const Bitmap &source, Kernel kernel = Kernel::Auto);

        // One row of halve(): two RGBA8 rows of the width in, one row half as wide out, for
        // halving an image as it streams past.
        static void halve_row(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, int width, Kernel kernel = Kernel::Auto);

        // True when the kernel can run on this machine.
        [[nodiscard]] static bool supports(Kernel kernel);

        // True while a level is still larger than the box on either side.
        [[nodiscard]] static bool exceeds(const Bitmap &level, int boxWidth, int boxHeight);

        [[nodiscard]] bool empty() const { return levels.empty(); }
        [[nodiscard]] int width() const { return levels.empty() ? 0 : levels.front()->width(); }
        [[nodiscard]] int height() const { return levels.empty() ? 0 : levels.front()->height(); }
        [[nodiscard]] std::size_t bytes() const;

        // The first level within the box, else the last.
        [[nodiscard]] std::size_t fitting(int boxWidth, int boxHeight) const;

        // The same pyramid without the levels above the one fitting the box.
        [[nodiscard]] Pyramid trimmed(int boxWidth, int boxHeight) const;
    };
}


#endif //TIV_IMAGE_PYRAMID_H
