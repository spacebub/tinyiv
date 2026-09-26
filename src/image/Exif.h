// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_EXIF_H
#define TIV_IMAGE_EXIF_H


#include <cstdint>
#include <span>

namespace tiv::Exif {

    // The orientation tag of a TIFF header block, 1 to 8, with 1 for anything missing or
    // malformed. A leading "Exif\0\0", as JPEG carries it, is skipped.
    [[nodiscard]] int orientation(std::span<const std::uint8_t> block);

}


#endif //TIV_IMAGE_EXIF_H
