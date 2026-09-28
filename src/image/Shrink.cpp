// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "image/Bitmap.h"
#include "image/Shrink.h"

namespace tiv {
    BoxShrink::BoxShrink(const int width, const int height, const int factor, Bitmap *target, const int from)
        : _target(target), _width(width), _height(height), _factor(factor), _y(from), _sums(target->pitch()), _counts(static_cast<std::size_t>(target->width())) {
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
    void BoxShrink::push(const std::span<const std::uint8_t> row) {
        if (_target->encoding() == Bitmap::Encoding::Pq) {
            push_pq(row);

            return;
        }

        std::size_t in = 0;

        for (int x = 0; x < _target->width(); ++x) {
            const int block = std::min(_factor, _width - (x * _factor));
            const std::size_t at = static_cast<std::size_t>(x) * Bitmap::CHANNELS;
            std::uint32_t red = 0;
            std::uint32_t green = 0;
            std::uint32_t blue = 0;
            std::uint32_t alpha = 0;

            for (int i = 0; i < block; ++i, in += Bitmap::CHANNELS) {
                red += row[in];
                green += row[in + 1];
                blue += row[in + 2];
                alpha += row[in + 3];
            }

            _sums[at] += red;
            _sums[at + 1] += green;
            _sums[at + 2] += blue;
            _sums[at + 3] += alpha;
        }

        advance();
    }

    void BoxShrink::push_pq(const std::span<const std::uint8_t> row) {
        std::size_t in = 0;

        for (int x = 0; x < _target->width(); ++x) {
            const int block = std::min(_factor, _width - (x * _factor));
            const std::size_t at = static_cast<std::size_t>(x) * Bitmap::CHANNELS;

            for (int i = 0; i < block; ++i, in += Bitmap::CHANNELS) {
                std::uint32_t word = 0;

                std::memcpy(&word, row.subspan(in, 4).data(), 4);

                const auto [red, green, blue, alpha] = Bitmap::unpack(word);

                _sums[at] += red;
                _sums[at + 1] += green;
                _sums[at + 2] += blue;
                _sums[at + 3] += alpha;
            }
        }

        advance();
    }

    void BoxShrink::advance() {
        ++_gathered;

        if (_upward ? _y % _factor == 0 : (_y + 1) % _factor == 0 || _y == _height - 1) {
            const std::span<std::uint8_t> out = _target->row(_y / _factor);
            const bool pq = _target->encoding() == Bitmap::Encoding::Pq;

            for (int x = 0; x < _target->width(); ++x) {
                const std::uint32_t count = _counts[static_cast<std::size_t>(x)] * static_cast<std::uint32_t>(_gathered);
                const std::size_t at = static_cast<std::size_t>(x) * Bitmap::CHANNELS;
                const auto mean = [&](const std::size_t c) {
                    return (_sums[at + c] + (count / 2)) / count;
                };

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
