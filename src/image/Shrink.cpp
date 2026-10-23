// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/Shrink.h"

namespace tiv {
    BoxShrink::BoxShrink(const int width, const int height, const int factor, Bitmap *target, const int from)
        : _target(target), _height(height), _factor(factor), _y(from),
          _sums(static_cast<std::size_t>(width) * Bitmap::CHANNELS),
          _counts(static_cast<std::size_t>(target->width())) {
        for (int x = 0; x < width; ++x) {
            ++_counts.at(static_cast<std::size_t>(x / factor));
        }
    }

    BoxShrink BoxShrink::upward(const int width, const int height, const int factor, Bitmap *target) {
        BoxShrink held(width, height, factor, target, height - 1);

        held._upward = true;

        return held;
    }

    void BoxShrink::skip() {
        advance();
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): the inner loop runs over gigapixels.
    // Rows add into a sum a byte, and the blocks are summed across once a row of them is complete.
    void BoxShrink::push(const std::span<const std::uint8_t> row) {
        if (_target->encoding() == Bitmap::Encoding::Pq) {
            push_pq(row);

            return;
        }

        Channels::widen_add(row.data(), _sums.data(), std::min(row.size(), _sums.size()));
        advance();
    }

    void BoxShrink::push_pq(const std::span<const std::uint8_t> row) {
        const std::size_t pixels = std::min(row.size(), _sums.size()) / Bitmap::CHANNELS;

        for (std::size_t x = 0; x < pixels; ++x) {
            std::uint32_t word = 0;

            std::memcpy(&word, row.subspan(x * Bitmap::CHANNELS, 4).data(), 4);

            const auto [red, green, blue, alpha] = Bitmap::unpack(word);
            const std::size_t at = x * Bitmap::CHANNELS;

            _sums[at] += red;
            _sums[at + 1] += green;
            _sums[at + 2] += blue;
            _sums[at + 3] += alpha;
        }

        advance();
    }

    void BoxShrink::advance() {
        ++_gathered;

        if (_upward ? _y % _factor == 0 : (_y + 1) % _factor == 0 || _y == _height - 1) {
            const std::span<std::uint8_t> out = _target->row(_y / _factor);
            const bool pq = _target->encoding() == Bitmap::Encoding::Pq;
            std::size_t in = 0;

            for (int x = 0; x < _target->width(); ++x) {
                const std::uint32_t columns = _counts[static_cast<std::size_t>(x)];
                const std::uint32_t count = std::max(columns * static_cast<std::uint32_t>(_gathered), 1U);
                std::array<std::uint32_t, Bitmap::CHANNELS> block{};

                for (std::uint32_t i = 0; i < columns; ++i, in += Bitmap::CHANNELS) {
                    for (std::size_t c = 0; c < Bitmap::CHANNELS; ++c) {
                        block.at(c) += _sums[in + c];
                    }
                }

                const auto mean = [&](const std::size_t c) { return (block.at(c) + (count / 2)) / count; };
                const std::size_t at = static_cast<std::size_t>(x) * Bitmap::CHANNELS;

                if (pq) {
                    const std::uint32_t word = Bitmap::pack(mean(0), mean(1), mean(2), mean(3));

                    std::memcpy(out.subspan(at, 4).data(), &word, 4);

                    continue;
                }

                for (std::size_t c = 0; c < Bitmap::CHANNELS; ++c) {
                    out[at + c] = static_cast<std::uint8_t>(mean(c));
                }
            }

            std::ranges::fill(_sums, 0U);
            _gathered = 0;
        }

        _y += _upward ? -1 : 1;
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}
