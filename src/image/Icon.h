// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_ICON_H
#define TIV_IMAGE_ICON_H


#include <cstdint>
#include <span>

#include "image/Bitmap.h"

namespace tiv::Icon {

    enum class Payload : std::uint8_t {
        Png,
        Jpeg2000,
        // The run length coded RGB planes of older ICNS files, with the alpha in an entry of its own.
        Packed,
        // A Windows bitmap without its file header.
        Dib,
    };

    struct Entry {
        Payload payload = Payload::Png;
        std::span<const std::uint8_t> data;
        // The alpha of a packed entry. Empty when the file has none, and the entry is opaque.
        std::span<const std::uint8_t> mask;
        int width = 0;
        int height = 0;
    };

    // The largest entry of the file, the one shown. False when it holds none this reads.
    bool largest_ico(std::span<const std::uint8_t> file, Entry *out);
    bool largest_icns(std::span<const std::uint8_t> file, Entry *out);

    bool unpack(const Entry &entry, Bitmap *out);

}


#endif //TIV_IMAGE_ICON_H
