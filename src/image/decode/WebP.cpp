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
#include <span>
#include <string>
#include <utility>

#include <webp/decode.h>

#include "image/Bitmap.h"
#include "image/Exif.h"
#include "image/Orient.h"
#include "image/decode/Support.h"
#include "image/decode/WebP.h"

namespace tiv::Decode {
    namespace {
        // The incremental WebP decoder is fed this much between abort checks.
        constexpr std::size_t WEBP_CHUNK = std::size_t{8} * 1024 * 1024;

        struct WebPIncremental {
            WebPIDecoder *decoder = nullptr;

            ~WebPIncremental() {
                if (decoder != nullptr) {
                    WebPIDelete(decoder);
                }
            }

            WebPIncremental() = default;
            WebPIncremental(const WebPIncremental &) = delete;
            WebPIncremental(WebPIncremental &&) = delete;
            WebPIncremental &operator=(const WebPIncremental &) = delete;
            WebPIncremental &operator=(WebPIncremental &&) = delete;
        };

        // The EXIF chunk of an extended file: https://developers.google.com/speed/webp/docs/riff_container
        int webp_orientation(const std::span<const std::uint8_t> data) {
            constexpr std::size_t FIRST_CHUNK = 12;
            constexpr std::size_t CHUNK_HEADER = 8;

            for (std::size_t at = FIRST_CHUNK; at + CHUNK_HEADER <= data.size();) {
                const std::size_t size = data[at + 4] | (data[at + 5] << 8) | (data[at + 6] << 16) | (static_cast<std::size_t>(data[at + 7]) << 24);

                if (at + CHUNK_HEADER + size > data.size()) {
                    break;
                }

                if (starts_with(data, "EXIF", at)) {
                    return Exif::orientation(data.subspan(at + CHUNK_HEADER, size));
                }

                at += CHUNK_HEADER + size + (size & 1);
            }

            return 1;
        }
    }

    bool WebP::probe(const std::span<const std::uint8_t> data, Decode::Info *info) {
        WebPBitstreamFeatures features;

        if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK || features.has_animation != 0) {
            return false;
        }

        info->orientation = webp_orientation(data);
        info->width = Orient::swaps(info->orientation) ? features.height : features.width;
        info->height = Orient::swaps(info->orientation) ? features.width : features.height;

        return true;
    }

    Direct WebP::load(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, const Decode::Abort *abort) {
        WebPDecoderConfig config;

        if (WebPInitDecoderConfig(&config) == 0 || WebPGetFeatures(data.data(), data.size(), &config.input) != VP8_STATUS_OK
            || config.input.has_animation != 0) {
            return Direct::Skip;
        }

        Bitmap held = Bitmap::allocate(config.input.width, config.input.height);

        config.options.use_threads = 1;
        config.output.colorspace = MODE_RGBA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = held.data();
        config.output.u.RGBA.stride = static_cast<int>(held.pitch());
        config.output.u.RGBA.size = held.bytes();

        if (abort == nullptr) {
            if (WebPDecode(data.data(), data.size(), &config) != VP8_STATUS_OK) {
                fail(error, file, "webp decode failed");

                return Direct::Failed;
            }
        } else {
            // Fed in chunks so an abort lands within a few dozen milliseconds. The data stays
            // mapped for the whole decode, so nothing is copied.
            WebPIncremental incremental;

            incremental.decoder = WebPIDecode(nullptr, 0, &config);

            if (incremental.decoder == nullptr) {
                return Direct::Skip;
            }

            VP8StatusCode status = VP8_STATUS_SUSPENDED;

            for (std::size_t fed = 0; fed < data.size() && status == VP8_STATUS_SUSPENDED;) {
                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return Direct::Failed;
                }

                fed = std::min(fed + WEBP_CHUNK, data.size());
                status = WebPIUpdate(incremental.decoder, data.data(), fed);
            }

            if (status != VP8_STATUS_OK) {
                fail(error, file, "webp decode failed");

                return Direct::Failed;
            }
        }

        *out = std::move(held);

        return Direct::Done;
    }
}
