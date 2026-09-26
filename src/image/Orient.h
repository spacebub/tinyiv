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


#include "image/Bitmap.h"

namespace tiv::Orient {

    // True for orientations 5 to 8, which swap width and height.
    [[nodiscard]] bool swaps(int orientation);

    // The bitmap as the EXIF orientation says it should be displayed. Orientation 1 is
    // handed back untouched.
    [[nodiscard]] Bitmap apply(Bitmap source, int orientation);

}


#endif //TIV_IMAGE_ORIENT_H
