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

    // RGBA8 to planes of red, green, blue and alpha, the alpha left out when null. With green apart,
    // red and blue are kept less green, as interleave() takes them.
    void deinterleave(const std::uint8_t *rgba, std::size_t pixels, bool greenApart, std::uint8_t *red,
                      std::uint8_t *green, std::uint8_t *blue, std::uint8_t *alpha);

    // Over the row of residuals, the sum of each byte's magnitude as a signed number, and the sum of
    // each byte's difference from the one before it, the first from zero: what compresses as it is
    // against what slope() would leave.
    void magnitudes(const std::uint8_t *row, std::size_t bytes, std::uint64_t *plain, std::uint64_t *sloped);

    // Each byte of the row less the one before it, in place, the first kept, which prefix() undoes.
    void slope(std::uint8_t *row, std::size_t bytes);

    // Each byte of the row added into the sum beside it.
    void widen_add(const std::uint8_t *row, std::uint32_t *sums, std::size_t bytes);

}


#endif //TIV_IMAGE_CHANNELS_H
