// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_HEIF_H
#define TIV_DECODE_HEIF_H


#include <cstdint>
#include <span>
#include <string_view>

#include "image/Tone.h"

namespace tiv::Heif {

    struct Colour {
        // The nclx colour of the primary item, which libvips does not pass on.
        Tone::Source tone;
        // A tmap item, or the auxiliary image Apple keeps its gain map in.
        bool gainMap = false;
    };

    // From the meta box of a HEIF or AVIF file: ISO/IEC 23008-12, ISO/IEC 14496-12.
    [[nodiscard]] Colour colour(std::span<const std::uint8_t> data);

    // The body of the first top level box of the type, empty when there is none. The JPEG XL
    // container is made of the same boxes.
    [[nodiscard]] std::span<const std::uint8_t> box(std::span<const std::uint8_t> data, std::string_view type);

}


#endif //TIV_DECODE_HEIF_H
