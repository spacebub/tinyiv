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
#include <filesystem>
#include <string>

#include <vips/vips8>

#include "image/Bitmap.h"
#include "image/Mapped.h"
#include "image/decode/Decode.h"
#include "image/decode/Jxl.h"
#include "image/decode/Support.h"
#include "image/decode/Vips.h"

namespace tiv {
    bool Decode::stream(const std::filesystem::path &file, const int rows, const Begin &begin, const Take &take, std::string *error, Abort *abort, const Tone::Display &display) {
        if (Mapped mapped; Mapped::open(file, &mapped) && sniff(mapped.data()) == Format::Jxl) {
            const Direct direct = Jxl::stream(mapped.data(), rows, begin, take, abort, display);

            if (direct != Direct::Skip) {
                if (direct == Direct::Failed) {
                    fail(error, file, aborted(abort) ? "aborted" : "jxl decode failed");
                }

                return direct == Direct::Done;
            }
        }

        Vips::ensure();

        try {
            // Sequential, so each band decodes as it is asked for and nothing above it stays.
            const vips::VImage image = vips::VImage::new_from_file(file.string().c_str(), vips::VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL));
            const bool alpha = image.has_alpha();
            const Vips::Prepared prepared = Vips::prepare(image, Vips::container_tone(file), display);
            const int width = prepared.image.width();
            const int height = prepared.image.height();

            if (!begin(width, height, alpha, prepared.encoding)) {
                fail(error, file, "stopped");

                return false;
            }

            Bitmap band = Bitmap::allocate(width, std::min(rows, height), prepared.encoding);

            for (int y = 0; y < height; y += rows) {
                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return false;
                }

                const int count = std::min(rows, height - y);
                const std::size_t bytes = band.pitch() * static_cast<std::size_t>(count);

                if (!Vips::write_rows(prepared, y, count, band.data(), abort)) {
                    fail(error, file, Vips::last_error());

                    return false;
                }

                if (!take(y, count, band.all().first(bytes))) {
                    fail(error, file, "stopped");

                    return false;
                }
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

    std::uint64_t Decode::stream_bytes(const std::filesystem::path &file, const int rows) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped) || sniff(mapped.data()) != Format::Jxl) {
            return 0;
        }

        return Jxl::stream_bytes(mapped.data(), rows);
    }
}
