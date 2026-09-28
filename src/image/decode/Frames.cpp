// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <vips/vips8>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"
#include "image/decode/Vips.h"

namespace tiv {
    namespace {
        using vips::VImage;

        // Browsers play frame delays this short at the default, and animations are made for browsers.
        constexpr int SHORTEST_DELAY = 10;
        constexpr int DEFAULT_DELAY = 100;

        // The smallest integer shrink that brings every frame together within the bytes.
        int frame_shrink(const int width, const int height, const int frames, const std::size_t maxBytes) {
            const auto bytes = [&](const int factor) {
                return static_cast<std::size_t>((width + factor - 1) / factor)
                       * static_cast<std::size_t>((height + factor - 1) / factor) * Bitmap::CHANNELS
                       * static_cast<std::size_t>(frames);
            };

            int factor = 1;

            while (bytes(factor) > maxBytes && factor < std::max(width, height)) {
                ++factor;
            }

            return factor;
        }

        int frame_delay(const std::vector<int> &delays, const int frame) {
            const auto at = static_cast<std::size_t>(frame);
            const int delay = at < delays.size() ? delays.at(at) : 0;

            return delay <= SHORTEST_DELAY ? DEFAULT_DELAY : delay;
        }
    }

    bool Decode::load_frames(const std::filesystem::path &file, const std::size_t maxBytes, std::vector<Frame> *out,
                             std::string *error, Abort *abort) {
        Vips::ensure();

        try {
            // Sequential, so the frames stream through one by one and the strip never decodes whole up front.
            VImage strip = VImage::new_from_file(file.string().c_str(),
                                                 VImage::option()->set("n", -1)->set("access", VIPS_ACCESS_SEQUENTIAL));
            const int height = vips_image_get_page_height(strip.get_image());
            const int frames = strip.height() / height;
            const std::vector<int> delays =
                    strip.get_typeof("delay") != 0 ? strip.get_array_int("delay") : std::vector<int>{};
            const int factor = frame_shrink(strip.width(), height, frames, maxBytes);
            const Tone::Source source = Vips::container_tone(file);
            std::vector<Frame> held;

            held.reserve(static_cast<std::size_t>(frames));

            for (int frame = 0; frame < frames; ++frame) {
                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return false;
                }

                VImage page = strip.crop(0, frame * height, strip.width(), height);

                if (factor > 1) {
                    page = page.shrink(factor, factor);
                }

                Frame next;

                if (!Vips::write_rgba(Vips::prepare(page, source), &next.bitmap, abort)) {
                    fail(error, file, Vips::last_error());

                    return false;
                }

                next.delay = frame_delay(delays, frame);
                held.push_back(std::move(next));
            }

            *out = std::move(held);

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
