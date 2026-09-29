// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_CHANNELS_H
#define TIV_IMAGE_CHANNELS_H


#include <cstddef>
#include <cstdint>

namespace tiv::Channels {

    // RGB8 to RGBA8, opaque.
    void expand(const std::uint8_t *rgb, std::uint8_t *rgba, int pixels);

    // RGBA8 to RGB8, the alpha dropped.
    void pack(const std::uint8_t *rgba, std::uint8_t *rgb, int pixels);

    // Each byte of the row less the one above it, the Up filter of PNG, which leaves small
    // numbers that compress well. Out may be the row itself.
    // Spec: https://www.w3.org/TR/png-3/#9Filter-types, filter type 2.
    void difference(const std::uint8_t *row, const std::uint8_t *above, std::uint8_t *out, std::size_t bytes);

    // Undoes difference() in place: each byte plus the one above it.
    void accumulate(std::uint8_t *row, const std::uint8_t *above, std::size_t bytes);

    // Undoes PNG's Sub filter in place: each byte plus every one before it in the row.
    // Spec: https://www.w3.org/TR/png-3/#9Filter-types, filter type 1.
    void prefix(std::uint8_t *row, std::size_t bytes);

    // Planes of red, green, blue and alpha to RGBA8, opaque without alpha. With green apart, red and
    // blue were kept less green, which is added back.
    void interleave(const std::uint8_t *red, const std::uint8_t *green, const std::uint8_t *blue,
                    const std::uint8_t *alpha, bool greenApart, std::uint8_t *rgba, std::size_t pixels);

}


#endif //TIV_IMAGE_CHANNELS_H
