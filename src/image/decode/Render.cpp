// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vips/vips8>

#include "image/Bitmap.h"
#include "image/Mapped.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"
#include "image/decode/Svg.h"
#include "image/decode/Vips.h"

namespace tiv {
    namespace {
        using vips::VImage;

        // librsvg sizes every filter and mask buffer to the whole document, and libvips renders
        // an SVG in tiles this size, each drawing the whole document again. So the part is cut
        // into pieces, each a document of its own that shows only the piece and fits one tile.
        constexpr int SVG_TILE = 2000;

        // A blur reaches past the piece it lands in, so this much around each is drawn and dropped.
        constexpr int SVG_REACH = 256;

        struct Part {
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
        };

        // One piece of the part as a document of its own, drawn with its reach and cropped back.
        bool render_svg_piece(const std::span<const std::uint8_t> document, const double scale,
                              const Decode::Size whole, const Part piece, Bitmap *out) {
            const int left = std::max(piece.x - SVG_REACH, 0);
            const int top = std::max(piece.y - SVG_REACH, 0);
            const int width = std::min(piece.x + piece.width + SVG_REACH, whole.width) - left;
            const int height = std::min(piece.y + piece.height + SVG_REACH, whole.height) - top;
            const std::string narrowed =
                    Svg::narrow(document, scale, whole.width, whole.height, left, top, width, height);

            if (narrowed.empty()) {
                return false;
            }

            try {
                const VImage drawn = VImage::new_from_buffer(narrowed.data(), narrowed.size(), "");

                if (drawn.width() != width || drawn.height() != height) {
                    return false;
                }

                return Decode::Vips::write_rgba(
                        Decode::Vips::prepare(drawn.crop(piece.x - left, piece.y - top, piece.width, piece.height), {}),
                        out, nullptr);
            } catch (const vips::VError &) {
                return false;
            }
        }

        // The pieces render on threads of their own, since libvips would draw them one by one.
        bool render_svg(const std::span<const std::uint8_t> document, const double scale, const Decode::Size whole,
                        const Part part, Bitmap *out, const Decode::Abort *abort) {
            constexpr int STEP = SVG_TILE - (2 * SVG_REACH);
            const int across = (part.width + STEP - 1) / STEP;
            const int down = (part.height + STEP - 1) / STEP;
            const int pieceWidth = (part.width + across - 1) / across;
            const int pieceHeight = (part.height + down - 1) / down;
            const int count = across * down;
            const int wanted = std::min(Decode::MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
            Bitmap target = Bitmap::allocate(part.width, part.height);
            std::atomic<int> next = 0;
            std::atomic<bool> failed = false;

            const auto work = [&] {
                for (int index = next++; index < count && !failed && !Decode::aborted(abort); index = next++) {
                    const int column = index % across;
                    const int row = index / across;
                    const Part piece{
                            .x = part.x + (column * pieceWidth),
                            .y = part.y + (row * pieceHeight),
                            .width = std::min(pieceWidth, part.width - (column * pieceWidth)),
                            .height = std::min(pieceHeight, part.height - (row * pieceHeight)),
                    };
                    Bitmap drawn;

                    if (!render_svg_piece(document, scale, whole, piece, &drawn)) {
                        failed = true;

                        break;
                    }

                    for (int y = 0; y < drawn.height(); ++y) {
                        std::ranges::copy(drawn.row(y), target.row((row * pieceHeight) + y)
                                                                .subspan(static_cast<std::size_t>(column * pieceWidth)
                                                                         * Bitmap::CHANNELS)
                                                                .begin());
                    }
                }
            };

            {
                std::vector<std::jthread> workers;

                for (int i = 1; i < std::min(count, std::max(wanted, 1)); ++i) {
                    workers.emplace_back(work);
                }

                work();
            }

            if (failed || Decode::aborted(abort)) {
                return false;
            }

            *out = std::move(target);

            return true;
        }
    }

    bool Decode::render(const std::filesystem::path &file, const double scale, const int x, const int y,
                        const int width, const int height, Bitmap *out, std::string *error, Abort *abort) {
        Vips::ensure();

        try {
            const VImage image = VImage::new_from_file(file.string().c_str(), VImage::option()->set("scale", scale));
            const int left = std::clamp(x, 0, image.width() - 1);
            const int top = std::clamp(y, 0, image.height() - 1);
            const int partWidth = std::clamp(width, 1, image.width() - left);
            const int partHeight = std::clamp(height, 1, image.height() - top);

            if (Mapped mapped; Mapped::open(file, &mapped) && sniff(mapped.data()) == Format::Svg) {
                if (render_svg(mapped.data(), scale, {.width = image.width(), .height = image.height()},
                               {.x = left, .y = top, .width = partWidth, .height = partHeight}, out, abort)) {
                    return true;
                }

                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return false;
                }
            }

            const VImage part = image.crop(left, top, partWidth, partHeight);

            if (!Vips::write_rgba(Vips::prepare(part, {}), out, abort)) {
                fail(error, file, Vips::last_error());

                return false;
            }

            return true;
        } catch (const vips::VError &) {
            if (abort != nullptr) {
                abort->disarm();
            }

            fail(error, file, Vips::last_error());

            return false;
        }
    }
}
