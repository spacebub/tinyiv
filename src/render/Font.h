// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_FONT_H
#define TIV_RENDER_FONT_H


#include <cstdint>
#include <span>
#include <string_view>

namespace tiv::Font {

constexpr int SIZE = 8;
constexpr int GLYPHS = 190;

// -1 for a code point drawn blank.
[[nodiscard]] int glyph(char32_t code);

// Bit x of row y is the pixel at (x, y).
[[nodiscard]] std::span<const std::uint8_t, SIZE> rows(int glyph);

// Bytes that are not UTF-8 come out one each.
[[nodiscard]] char32_t next(std::string_view &text);

}


#endif //TIV_RENDER_FONT_H
