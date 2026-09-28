// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_ORIENT_H
#define TIV_IMAGE_ORIENT_H


namespace tiv::Orient {

    // Orientations are the eight EXIF values: how the image as stored turns and mirrors to be
    // shown. Bitmaps stay as stored, and the orientation is applied when they are drawn.
    // https://www.cipa.jp/std/documents/e/DC-X008-Translation-2019-E.pdf, tag 0x0112.

    // Turns and flips of the image as shown, to compose with the orientation it has.
    constexpr int FLIP_HORIZONTAL = 2;
    constexpr int FLIP_VERTICAL = 4;
    constexpr int TURN_RIGHT = 6;
    constexpr int TURN_LEFT = 8;

    // True for orientations 5 to 8, which swap width and height.
    [[nodiscard]] bool swaps(int orientation);

    // Drawing applies the mirror, left to right, first and then the quarter turns clockwise.
    [[nodiscard]] bool mirrors(int orientation);
    [[nodiscard]] int quarters(int orientation);

    // Inner applied first, then outer.
    [[nodiscard]] int compose(int outer, int inner);
    [[nodiscard]] int inverse(int orientation);

}


#endif //TIV_IMAGE_ORIENT_H
