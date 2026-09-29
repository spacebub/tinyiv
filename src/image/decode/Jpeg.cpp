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
#include <atomic>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <jpeglib.h>

#include "image/Bitmap.h"
#include "image/Exif.h"
#include "image/Orient.h"
#include "image/Tone.h"
#include "image/decode/GainMap.h"
#include "image/decode/Jpeg.h"
#include "image/decode/JpegBands.h"
#include "image/decode/JpegHandle.h"
#include "image/decode/Support.h"

namespace tiv::Decode {
    namespace {
        // XMP travels in APP1 too, so the Exif one is looked for.
        int jpeg_orientation(const jpeg_decompress_struct &info) {
            for (const jpeg_marker_struct *marker = info.marker_list; marker != nullptr; marker = marker->next) {
                const std::span<const std::uint8_t> data(marker->data, marker->data_length);

                if (marker->marker == JPEG_APP0 + 1 && starts_with(data, std::string_view("Exif\0\0", 6))) {
                    return Exif::orientation(data);
                }
            }

            return 1;
        }

        constexpr unsigned JPEG_DENOM = 8;

        // Below this, one thread decodes about as fast as several would start.
        constexpr std::int64_t BANDED_PIXELS = std::int64_t{4} * 1000 * 1000;

        Size jpeg_scaled(const jpeg_decompress_struct &info, const unsigned num) {
            return {
                    .width = static_cast<int>(((static_cast<unsigned long>(info.image_width) * num) + JPEG_DENOM - 1)
                                              / JPEG_DENOM),
                    .height = static_cast<int>(((static_cast<unsigned long>(info.image_height) * num) + JPEG_DENOM - 1)
                                               / JPEG_DENOM),
            };
        }

        // Only the powers of two have SIMD inverse transforms in libjpeg-turbo. The others
        // decode slower than the whole image.
        constexpr std::array<unsigned, 4> JPEG_SCALES = {1, 2, 4, 8};

        // The smallest scale that still covers the fitted size, or when forced, the largest
        // that stays within the box.
        unsigned jpeg_scale(const jpeg_decompress_struct &info, const Size box, const int orientation, const Fit fit) {
            const int boxWidth = Orient::swaps(orientation) ? box.height : box.width;
            const int boxHeight = Orient::swaps(orientation) ? box.width : box.height;

            if (fit == Fit::Force) {
                for (const unsigned num : std::views::reverse(JPEG_SCALES)) {
                    const Size size = jpeg_scaled(info, num);

                    if (size.width <= boxWidth && size.height <= boxHeight) {
                        return num;
                    }
                }

                return JPEG_SCALES.front();
            }

            for (const unsigned num : JPEG_SCALES) {
                const Size size = jpeg_scaled(info, num);

                if (size.width >= boxWidth && size.height >= boxHeight) {
                    return num;
                }
            }

            return JPEG_DENOM;
        }

        // The caller sizes target from output_width and output_height.
        bool jpeg_read(JpegHandle &handle, Bitmap &target, std::vector<JSAMPROW> &rows, const Abort *abort) {
            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            if (setjmp(handle.error.jump) != 0) {
                return false;
            }

            if (jpeg_start_decompress(&handle.info) == 0) {
                return false;
            }

            const auto batch = static_cast<int>(std::max(handle.info.rec_outbuf_height, 1));

            while (handle.info.output_scanline < handle.info.output_height) {
                const int at = static_cast<int>(handle.info.output_scanline);

                if (at % ABORT_ROWS == 0 && aborted(abort)) {
                    return false;
                }

                const int count = std::min(batch, target.height() - at);

                for (int i = 0; i < count; ++i) {
                    rows.at(static_cast<std::size_t>(i)) = target.row(at + i).data();
                }

                if (jpeg_read_scanlines(&handle.info, rows.data(), static_cast<JDIMENSION>(count)) == 0) {
                    return false;
                }
            }

            return jpeg_finish_decompress(&handle.info) != 0;
        }

        Direct jpeg_decode(const std::filesystem::path &file, const std::span<const std::uint8_t> data,
                           const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Abort *abort,
                           const Fit fit) {
            JpegHandle handle;

            if (!jpeg_open(handle, data)) {
                fail(error, file, "jpeg header unreadable");

                return Direct::Failed;
            }

            if (handle.info.jpeg_color_space == JCS_CMYK || handle.info.jpeg_color_space == JCS_YCCK) {
                return Direct::Skip;
            }

            const int orientation = jpeg_orientation(handle.info);
            const auto width = static_cast<int>(handle.info.image_width);
            const auto height = static_cast<int>(handle.info.image_height);
            Size want{.width = boxWidth, .height = boxHeight};

            if (fit != Fit::Force) {
                const int shownWidth = Orient::swaps(orientation) ? height : width;
                const int shownHeight = Orient::swaps(orientation) ? width : height;

                want = fitted(shownWidth, shownHeight, boxWidth, boxHeight);
            }

            handle.info.scale_num = jpeg_scale(handle.info, want, orientation, fit);
            handle.info.scale_denom = JPEG_DENOM;
            handle.info.out_color_space = JCS_EXT_RGBA;
            handle.info.dct_method = JDCT_ISLOW;

            jpeg_calc_output_dimensions(&handle.info);

            Bitmap held = Bitmap::allocate(static_cast<int>(handle.info.output_width),
                                           static_cast<int>(handle.info.output_height));

            // Restart markers split the image into bands that decode side by side. Should the bands fail
            // for any reason but an abort, the plain decode below still gets its turn.
            if (JpegBands bands; static_cast<std::int64_t>(width) * height >= BANDED_PIXELS
                                 && JpegBands::index(data, &bands)
                                 && bands.decode(data, 0, height, handle.info.scale_num, &held, MAX_THREADS, abort)) {
                *out = std::move(held);

                return Direct::Done;
            }

            std::vector<JSAMPROW> rows(static_cast<std::size_t>(std::max(handle.info.rec_outbuf_height, 1)) + 1);

            if (aborted(abort) || !jpeg_read(handle, held, rows, abort)) {
                fail(error, file, aborted(abort) ? "aborted" : "jpeg decode failed");

                return Direct::Failed;
            }

            *out = std::move(held);

            return Direct::Done;
        }

        // Bands of rows of the height on up to MAX_THREADS threads, until an abort.
        void parallel_rows(const int height, const std::function<void(int from, int to)> &each, const Abort *abort) {
            const int bands = (height + ABORT_ROWS - 1) / ABORT_ROWS;
            const int wanted = std::min(MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
            std::atomic<int> next = 0;

            const auto work = [&] {
                for (int band = next++; band < bands && !aborted(abort); band = next++) {
                    each(band * ABORT_ROWS, std::min((band + 1) * ABORT_ROWS, height));
                }
            };

            std::vector<std::jthread> workers;

            for (int i = 1; i < std::min(bands, std::max(wanted, 1)); ++i) {
                workers.emplace_back(work);
            }

            work();
        }

        // The base rendition lifted by its gain map as far as the display's headroom goes, into
        // PQ in place. Cut short by an abort, it leaves rows of both encodings.
        bool jpeg_lift(const std::span<const std::uint8_t> data, const Tone::Display &display, Bitmap *base,
                       const Abort *abort) {
            GainMap::Jpeg gain;
            Bitmap map;

            if (!GainMap::find_jpeg(data, &gain)) {
                return false;
            }

            const float weight = gain.metadata.weight(std::log2(display.headroom));
            constexpr int WHOLE = std::numeric_limits<int>::max();

            if (weight == 0.0F
                || jpeg_decode("gain map", gain.image, WHOLE, WHOLE, &map, nullptr, abort, Fit::Cheap)
                           != Direct::Done) {
                return false;
            }

            const GainMap::Applier applier(gain.metadata, &map, base->width(), base->height(), weight);
            const Tone::Mapper mapper({}, display);

            parallel_rows(
                    base->height(),
                    [&](const int from, const int to) {
                        for (int y = from; y < to; ++y) {
                            const std::span<std::uint8_t> row = base->row(y);

                            mapper.map(std::span<const std::uint8_t>(row), Bitmap::CHANNELS, row,
                                       [&applier, y](const std::size_t first, const std::span<float> rgba) {
                                           applier.apply(static_cast<int>(first), y, rgba);
                                       });
                        }
                    },
                    abort);

            base->set_encoding(Bitmap::Encoding::Pq);

            return !aborted(abort);
        }
    }

    bool Jpeg::probe(const std::span<const std::uint8_t> data, Info *info) {
        JpegHandle handle;

        if (!jpeg_open(handle, data)) {
            return false;
        }

        info->orientation = jpeg_orientation(handle.info);

        const auto width = static_cast<int>(handle.info.image_width);
        const auto height = static_cast<int>(handle.info.image_height);

        info->width = Orient::swaps(info->orientation) ? height : width;
        info->height = Orient::swaps(info->orientation) ? width : height;

        GainMap::Jpeg gain;

        info->hdr = GainMap::find_jpeg(data, &gain);

        return true;
    }

    Direct Jpeg::load(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth,
                      const int boxHeight, Bitmap *out, std::string *error, const Abort *abort, const Fit fit,
                      const Tone::Display &display) {
        const Direct direct = jpeg_decode(file, data, boxWidth, boxHeight, out, error, abort, fit);

        if (direct == Direct::Done && display.hdr() && !jpeg_lift(data, display, out, abort) && aborted(abort)) {
            *out = {};
            fail(error, file, "aborted");

            return Direct::Failed;
        }

        return direct;
    }
}
