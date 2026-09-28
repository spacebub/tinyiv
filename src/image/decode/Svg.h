// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_SVG_H
#define TIV_DECODE_SVG_H


#include <cstdint>
#include <span>
#include <string>

namespace tiv::Svg {

    // The document rewritten to show only part of itself. libvips renders the whole of it at
    // the scale to wholeWidth by wholeHeight, the part is in those pixels and the rewritten
    // document renders to exactly its size. Empty when the root element cannot be rewritten.
    [[nodiscard]] std::string narrow(std::span<const std::uint8_t> document, double scale, int wholeWidth, int wholeHeight, int x, int y, int width, int height);

}


#endif //TIV_DECODE_SVG_H
