// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_ANIMATION_H
#define TIV_IMAGE_ANIMATION_H


#include <cstddef>
#include <memory>
#include <vector>

#include "image/Pyramid.h"

namespace tiv {
    // Every frame of an animated image, in order, each with how long it stays up.
    struct Animation {
        struct Frame {
            std::shared_ptr<const Pyramid> pyramid;
            // Milliseconds.
            int delay = 0;
        };

        std::vector<Frame> frames;

        [[nodiscard]] std::size_t bytes() const;
        // Milliseconds, once through.
        [[nodiscard]] long duration() const;
    };
}


#endif //TIV_IMAGE_ANIMATION_H
