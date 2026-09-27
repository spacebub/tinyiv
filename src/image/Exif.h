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


#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tiv::Exif {

    // Length bytes at the offset become the bytes given.
    struct Splice {
        std::size_t at = 0;
        std::size_t length = 0;
        std::vector<std::uint8_t> bytes;
    };

    // The orientation tag of a TIFF header block, 1 to 8, with 1 for anything missing or
    // malformed. A leading "Exif\0\0", as JPEG carries it, is skipped.
    [[nodiscard]] int orientation(std::span<const std::uint8_t> block);

    // What makes IFD0 of a TIFF header block say the orientation, relative to the block. A tag
    // already there is patched. Otherwise IFD0 is copied to the end with the tag added, and
    // writing 1 over such a copy later puts the original back, so every change undoes to the
    // same bytes. False when the block is not TIFF or the tag is not a single SHORT.
    // https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf, section 2.
    bool orient(std::span<const std::uint8_t> block, int orientation, std::vector<Splice> *out);

    // The smallest TIFF header block that holds the orientation.
    [[nodiscard]] std::vector<std::uint8_t> minimal(int orientation);

    // Splices are sorted and do not overlap.
    [[nodiscard]] std::vector<std::uint8_t> apply(std::span<const std::uint8_t> data, const std::vector<Splice> &splices);

}


#endif //TIV_IMAGE_EXIF_H
