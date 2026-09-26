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
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "image/Bitmap.h"
#include "image/Orient.h"

namespace tiv {
    namespace {
        constexpr int BLOCK = 64;
        constexpr int MAX_THREADS = 8;
        constexpr int ROWS_PER_THREAD = 256;

        struct Mapping {
            // Source x = a * x + b * y + c, and likewise for y, in output coordinates.
            int ax = 1;
            int bx = 0;
            int cx = 0;
            int ay = 0;
            int by = 1;
            int cy = 0;
        };

        Mapping mapping(const int orientation, const int outWidth, const int outHeight) {
            const int w = outWidth - 1;
            const int h = outHeight - 1;

            switch (orientation) {
                case 2:
                    return {-1, 0, w, 0, 1, 0};
                case 3:
                    return {-1, 0, w, 0, -1, h};
                case 4:
                    return {1, 0, 0, 0, -1, h};
                case 5:
                    return {0, 1, 0, 1, 0, 0};
                case 6:
                    return {0, 1, 0, -1, 0, w};
                case 7:
                    return {0, -1, h, -1, 0, w};
                case 8:
                    return {0, -1, h, 1, 0, 0};
                default:
                    return {};
            }
        }

        std::uint32_t pixel(const Bitmap &source, const int x, const int y) {
            std::uint32_t held = 0;

            std::memcpy(&held, source.row(y).subspan(static_cast<std::size_t>(x) * Bitmap::CHANNELS, Bitmap::CHANNELS).data(), sizeof held);

            return held;
        }

        void map_rows(const Bitmap &source, Bitmap &target, const Mapping map, const int from, const int to) {
            for (int y0 = from; y0 < to; y0 += BLOCK) {
                const int y1 = std::min(y0 + BLOCK, to);

                for (int x0 = 0; x0 < target.width(); x0 += BLOCK) {
                    const int x1 = std::min(x0 + BLOCK, target.width());

                    for (int y = y0; y < y1; ++y) {
                        const std::span<std::uint8_t> out = target.row(y);

                        for (int x = x0; x < x1; ++x) {
                            const int sx = (map.ax * x) + (map.bx * y) + map.cx;
                            const int sy = (map.ay * x) + (map.by * y) + map.cy;
                            const std::uint32_t value = pixel(source, sx, sy);

                            std::memcpy(out.subspan(static_cast<std::size_t>(x) * Bitmap::CHANNELS, Bitmap::CHANNELS).data(), &value, sizeof value);
                        }
                    }
                }
            }
        }
    }

    bool Orient::swaps(const int orientation) {
        return orientation >= 5 && orientation <= 8;
    }

    Bitmap Orient::apply(Bitmap source, const int orientation) {
        if (orientation <= 1 || orientation > 8 || source.empty()) {
            return source;
        }

        const int outWidth = swaps(orientation) ? source.height() : source.width();
        const int outHeight = swaps(orientation) ? source.width() : source.height();
        Bitmap target = Bitmap::allocate(outWidth, outHeight);
        const Mapping map = mapping(orientation, outWidth, outHeight);

        const int wanted = std::min(MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
        const int threads = std::clamp(outHeight / ROWS_PER_THREAD, 1, std::max(wanted, 1));
        const int band = (outHeight + threads - 1) / threads;

        {
            std::vector<std::jthread> workers;

            for (int from = 0; from < outHeight; from += band) {
                workers.emplace_back(map_rows, std::cref(source), std::ref(target), map, from, std::min(from + band, outHeight));
            }
        }

        return target;
    }
}
