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

}


#endif //TIV_IMAGE_CHANNELS_H
