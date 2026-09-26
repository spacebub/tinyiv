// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <numeric>

#include "image/Animation.h"

namespace tiv {
    std::size_t Animation::bytes() const {
        return std::accumulate(frames.begin(), frames.end(), std::size_t{0}, [](const std::size_t sum, const Frame &frame) {
            return sum + frame.pyramid->bytes();
        });
    }

    long Animation::duration() const {
        return std::accumulate(frames.begin(), frames.end(), 0L, [](const long sum, const Frame &frame) { return sum + frame.delay; });
    }
}
