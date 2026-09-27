// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>

#include "image/Orient.h"

namespace tiv {
    namespace {
        // By mirror, then by quarter turns.
        constexpr std::array<std::array<int, 4>, 2> ORIENTATIONS = {{{1, 6, 3, 8}, {2, 7, 4, 5}}};

        int valid(const int orientation) {
            return orientation >= 1 && orientation <= 8 ? orientation : 1;
        }

        int orientation_of(const bool mirror, const int quarters) {
            return ORIENTATIONS.at(mirror ? 1 : 0).at(static_cast<std::size_t>(((quarters % 4) + 4) % 4));
        }
    }

    bool Orient::swaps(const int orientation) {
        return orientation >= 5 && orientation <= 8;
    }

    bool Orient::mirrors(const int orientation) {
        const int held = valid(orientation);

        return held == 2 || held == 4 || held == 5 || held == 7;
    }

    int Orient::quarters(const int orientation) {
        constexpr std::array<int, 9> TURNS = {0, 0, 0, 2, 2, 3, 1, 1, 3};

        return TURNS.at(static_cast<std::size_t>(valid(orientation)));
    }

    // A mirror before a turn is the opposite turn before the mirror.
    int Orient::compose(const int outer, const int inner) {
        if (mirrors(outer)) {
            return orientation_of(!mirrors(inner), quarters(outer) - quarters(inner));
        }

        return orientation_of(mirrors(inner), quarters(outer) + quarters(inner));
    }

    int Orient::inverse(const int orientation) {
        return mirrors(orientation) ? valid(orientation) : orientation_of(false, -quarters(orientation));
    }
}
