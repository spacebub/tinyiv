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
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <jpeglib.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <png.h>
#include <vips/vips8>
#include <webp/decode.h>

#include "image/Bitmap.h"
#include "image/Bmp.h"
#include "image/Decode.h"
#include "image/Exif.h"
#include "image/GainMap.h"
#include "image/Heif.h"
#include "image/Icon.h"
#include "image/Mapped.h"
#include "image/Orient.h"
#include "image/Shrink.h"
#include "image/Svg.h"
#include "image/Tone.h"

namespace tiv {
    namespace {
        using vips::VImage;

        constexpr int MAX_VIPS_THREADS = 8;

        // The incremental WebP decoder is fed this much between abort checks.
        constexpr std::size_t WEBP_CHUNK = std::size_t{8} * 1024 * 1024;

        // The direct decoders check for an abort every this many rows.
        constexpr int ABORT_ROWS = 64;

        // What a band of wide PNG rows waiting for the tone mapper may take.
        constexpr std::size_t PNG_BAND_BYTES = std::size_t{1} << 20;

        // Browsers play frame delays this short at the default, and animations are made for browsers.
        constexpr int SHORTEST_DELAY = 10;
        constexpr int DEFAULT_DELAY = 100;

        constexpr std::array<std::string_view, 34> SUFFIXES = {
                ".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".webp", ".jxl", ".gif", ".bmp", ".ico", ".icns", ".tif", ".tiff",
                ".heic", ".heif", ".avif", ".svg", ".svgz", ".pdf", ".jp2", ".j2k", ".jpx", ".exr", ".hdr",
                ".ppm", ".pgm", ".pbm", ".pnm", ".pfm", ".fits", ".fit", ".nii", ".v", ".vips",
        };

        struct Name {
            std::string_view loader;
            std::string_view format;
        };

        constexpr std::array NAMES = {
                Name{"jpeg", "JPEG"},
                Name{"png", "PNG"},
                Name{"webp", "WEBP"},
                Name{"tiff", "TIFF"},
                Name{"heif", "HEIF"},
                Name{"jxl", "JXL"},
                Name{"gif", "GIF"},
                Name{"svg", "SVG"},
                Name{"pdf", "PDF"},
                Name{"jp2k", "JP2"},
                Name{"dcraw", "RAW"},
                Name{"openexr", "EXR"},
                Name{"rad", "HDR"},
                Name{"ppm", "PPM"},
                Name{"vips", "VIPS"},
        };

        enum class Direct : std::uint8_t {
            Done,
            Failed,
            // Not this decoder's format, or a variant it leaves to libvips.
            Skip,
        };

        struct Size {
            int width = 0;
            int height = 0;
        };

        std::string_view format_name(const Decode::Format kind) {
            switch (kind) {
                case Decode::Format::Jpeg:
                    return "JPEG";
                case Decode::Format::Png:
                    return "PNG";
                case Decode::Format::WebP:
                    return "WEBP";
                case Decode::Format::Jxl:
                    return "JXL";
                case Decode::Format::Gif:
                    return "GIF";
                case Decode::Format::Bmp:
                    return "BMP";
                case Decode::Format::Ico:
                    return "ICO";
                case Decode::Format::Icns:
                    return "ICNS";
                case Decode::Format::Tiff:
                    return "TIFF";
                case Decode::Format::Heif:
                    return "HEIF";
                case Decode::Format::Svg:
                    return "SVG";
                case Decode::Format::Pdf:
                    return "PDF";
                case Decode::Format::Other:
                    break;
            }

            return {};
        }

        void fail(std::string *error, const std::filesystem::path &file, const std::string_view why) {
            if (error != nullptr) {
                *error = file.string() + ": " + std::string(why);
            }
        }

        bool aborted(const Decode::Abort *abort) {
            return abort != nullptr && abort->requested();
        }

        // The size the image has once fitted in the box, never enlarged.
        Size fitted(const int width, const int height, const int boxWidth, const int boxHeight) {
            if (width <= boxWidth && height <= boxHeight) {
                return {width, height};
            }

            const double shrink = std::min(static_cast<double>(boxWidth) / width, static_cast<double>(boxHeight) / height);

            return {std::max(static_cast<int>(std::floor(width * shrink)), 1), std::max(static_cast<int>(std::floor(height * shrink)), 1)};
        }

        bool starts_with(const std::span<const std::uint8_t> head, const std::string_view magic, const std::size_t at = 0) {
            if (head.size() < at + magic.size()) {
                return false;
            }

            return std::equal(magic.begin(), magic.end(), head.begin() + static_cast<std::ptrdiff_t>(at), [](const char a, const std::uint8_t b) {
                return static_cast<std::uint8_t>(a) == b;
            });
        }

        // --- libvips ---

        void drop_log(const gchar * /*domain*/, GLogLevelFlags /*level*/, const gchar * /*message*/, gpointer /*data*/) {
        }

        struct VipsState {
            std::once_flag once;
            bool started = false;
        };

        VipsState &vips_state() {
            static VipsState held;

            return held;
        }

        void ensure_vips() {
            VipsState &state = vips_state();

            std::call_once(state.once, [&state] {
                // libvips warns about every optional module missing from the system, on every launch.
                g_log_set_handler("VIPS", G_LOG_LEVEL_WARNING, drop_log, nullptr);

                if (VIPS_INIT("tinyiv") != 0) {
                    return;
                }

                // The operation cache would keep decoded images alive after the viewer has moved on.
                vips_cache_set_max(0);

                // Decoders are single threaded, so a pool the size of the machine mostly waits.
                vips_concurrency_set(std::min(vips_concurrency_get(), MAX_VIPS_THREADS));

                state.started = true;
            });
        }

        std::string vips_error() {
            std::string held = vips_error_buffer();

            vips_error_clear();

            while (!held.empty() && held.back() == '\n') {
                held.pop_back();
            }

            return held;
        }

        // Pages of these formats are frames, where in a TIFF or a PDF they are separate images.
        bool animates(const Decode::Format kind) {
            return kind == Decode::Format::Gif || kind == Decode::Format::WebP;
        }

        std::string vips_format_name(const VImage &image) {
            if (image.get_typeof(VIPS_META_LOADER) == 0) {
                return {};
            }

            std::string_view loader = image.get_string(VIPS_META_LOADER);

            if (const std::size_t cut = loader.find("load"); cut != std::string_view::npos) {
                loader = loader.substr(0, cut);
            }

            for (const Name &name : NAMES) {
                if (name.loader == loader) {
                    return std::string(name.format);
                }
            }

            std::string held(loader);

            for (char &c : held) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }

            return held;
        }

        VImage to_rgba(VImage image) {
            if (image.interpretation() != VIPS_INTERPRETATION_sRGB) {
                image = image.colourspace(VIPS_INTERPRETATION_sRGB);
            }

            if (image.format() != VIPS_FORMAT_UCHAR) {
                image = image.cast(VIPS_FORMAT_UCHAR);
            }

            if (!image.has_alpha()) {
                image = image.addalpha();
            }

            if (image.bands() > Bitmap::CHANNELS) {
                image = image.extract_band(0, VImage::option()->set("n", Bitmap::CHANNELS));
            }

            return image;
        }

        bool floating(const VImage &image) {
            return image.coding() == VIPS_CODING_RAD || image.format() == VIPS_FORMAT_FLOAT || image.format() == VIPS_FORMAT_DOUBLE;
        }

        // An image on its way to RGBA8: converted by libvips, or, when HDR, left in wide
        // samples of 1 to 4 bands for the tone mapper.
        struct Prepared {
            VImage image;
            std::optional<Tone::Mapper> mapper;
            Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
        };

        // Float samples are linear light whatever the file says. Wider integers carry their
        // transfer in the container, which the caller has read.
        Prepared prepare(VImage image, Tone::Source source, const Tone::Display &display = {}) {
            if (image.coding() == VIPS_CODING_RAD) {
                image = image.rad2float();
            }

            if (floating(image)) {
                source = {Tone::Transfer::Linear, Tone::Primaries::Bt709};
            }

            if (!source.hdr()) {
                return {to_rgba(image), std::nullopt, Bitmap::Encoding::Srgb};
            }

            if (image.bands() > Bitmap::CHANNELS) {
                image = image.extract_band(0, VImage::option()->set("n", Bitmap::CHANNELS));
            }

            if (source.transfer == Tone::Transfer::Linear) {
                image = image.cast(VIPS_FORMAT_FLOAT);
            } else if (image.format() == VIPS_FORMAT_UCHAR) {
                image = image.linear(257.0, 0.0).cast(VIPS_FORMAT_USHORT);
            } else if (image.format() != VIPS_FORMAT_USHORT) {
                image = image.cast(VIPS_FORMAT_USHORT);
            }

            return {image, Tone::Mapper(source, display), display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb};
        }

        // Where libvips hands over the rows of an HDR image, in order, for the tone mapper.
        struct MappedSink {
            const Tone::Mapper *mapper = nullptr;
            std::uint8_t *target = nullptr;
            int bands = 0;
        };

        // A region this large maps on several threads, since libvips hands regions over one at a time.
        constexpr std::size_t PARALLEL_PIXELS = std::size_t{1} << 20;

        template <typename Sample>
        void map_rows(VipsRegion *region, const VipsRect *area, const MappedSink *sink, const int from, const int to) {
            const auto samples = static_cast<std::size_t>(area->width) * static_cast<std::size_t>(sink->bands);
            const std::size_t pitch = static_cast<std::size_t>(area->width) * Bitmap::CHANNELS;

            for (int y = from; y < to; ++y) {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast): libvips hands the region over as raw rows.
                const std::span in(reinterpret_cast<const Sample *>(VIPS_REGION_ADDR(region, area->left, y)), samples);
                const std::span out(sink->target + (pitch * static_cast<std::size_t>(y)), pitch);
                // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)

                sink->mapper->map(in, sink->bands, out);
            }
        }

        template <typename Sample>
        int map_region(VipsRegion *region, VipsRect *area, void *opaque) {
            const auto *sink = static_cast<const MappedSink *>(opaque);
            const std::size_t pixels = static_cast<std::size_t>(area->width) * static_cast<std::size_t>(area->height);
            const int wanted = std::min(MAX_VIPS_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
            const int threads = pixels >= PARALLEL_PIXELS ? std::clamp(wanted, 1, area->height) : 1;
            const int slice = (area->height + threads - 1) / threads;
            const int bottom = area->top + area->height;

            {
                std::vector<std::jthread> workers;

                for (int from = area->top + slice; from < bottom; from += slice) {
                    workers.emplace_back(map_rows<Sample>, region, area, sink, from, std::min(from + slice, bottom));
                }

                map_rows<Sample>(region, area, sink, area->top, std::min(area->top + slice, bottom));
            }

            return 0;
        }

        // Count rows from y into RGBA8 at target, packed.
        bool write_rows(const Prepared &prepared, const int y, const int count, std::uint8_t *target, Decode::Abort *abort) {
            const VImage &image = prepared.image;
            const VImage part = y == 0 && count == image.height() ? image : image.crop(0, y, image.width(), count);

            if (abort != nullptr) {
                abort->arm(part.get_image());
            }

            bool written = false;

            if (prepared.mapper) {
                // One pass, which sequential loaders need. The wide samples never take more than the regions in flight.
                MappedSink sink{&*prepared.mapper, target, image.bands()};

                written = vips_sink_disc(part.get_image(), image.format() == VIPS_FORMAT_FLOAT ? map_region<float> : map_region<std::uint16_t>, &sink) == 0;
            } else {
                const std::size_t bytes = static_cast<std::size_t>(image.width()) * Bitmap::CHANNELS * static_cast<std::size_t>(count);
                const VImage packed = VImage::new_from_memory(target, bytes, image.width(), count, Bitmap::CHANNELS, VIPS_FORMAT_UCHAR);

                written = vips_image_write(part.get_image(), packed.get_image()) == 0;
            }

            if (abort != nullptr) {
                abort->disarm();
            }

            return written;
        }

        bool probe_vips(const std::filesystem::path &file, Decode::Info *info, std::string *error) {
            ensure_vips();

            try {
                const VImage image = VImage::new_from_file(file.string().c_str());
                const bool swapped = vips_image_get_orientation_swap(image.get_image()) != 0;

                info->width = swapped ? image.height() : image.width();
                info->height = swapped ? image.width() : image.height();
                info->orientation = vips_image_get_orientation(image.get_image());
                info->frames = animates(info->kind) ? std::max(vips_image_get_n_pages(image.get_image()), 1) : 1;
                info->hdr = info->hdr || floating(image);

                if (info->format.empty()) {
                    info->format = vips_format_name(image);
                }

                return true;
            } catch (const vips::VError &) {
                fail(error, file, vips_error());

                return false;
            }
        }

        enum class Via : std::uint8_t {
            // The image as it is.
            Whole,
            // Rendered or shrunk on load to the box, for formats that do that cheaply.
            Thumbnail,
            // Rendered to fit the box, larger or smaller, for formats drawn from shapes.
            Scaled,
            // Streamed through an integer box shrink, so memory is the output and not the input.
            Shrink,
        };

        // The smallest integer shrink that brings the size within the box.
        int shrink_factor(const int width, const int height, const int boxWidth, const int boxHeight) {
            int factor = 1;

            while ((width + factor - 1) / factor > boxWidth || (height + factor - 1) / factor > boxHeight) {
                ++factor;
            }

            return factor;
        }

        bool write_rgba(const Prepared &prepared, Bitmap *out, Decode::Abort *abort) {
            Bitmap held = Bitmap::allocate(prepared.image.width(), prepared.image.height(), prepared.encoding);

            if (!write_rows(prepared, 0, held.height(), held.data(), abort)) {
                return false;
            }

            *out = std::move(held);

            return true;
        }

        bool load_vips(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Decode::Abort *abort, const Via via, const Tone::Source source,
                       const Tone::Display &display) {
            ensure_vips();

            try {
                VImage image;

                if (via == Via::Thumbnail || via == Via::Scaled) {
                    const VipsSize size = via == Via::Scaled ? VIPS_SIZE_BOTH : VIPS_SIZE_DOWN;
                    const bool swapped = vips_image_get_orientation_swap(VImage::new_from_file(file.string().c_str()).get_image()) != 0;

                    image = VImage::thumbnail(file.string().c_str(), swapped ? boxHeight : boxWidth,
                                              VImage::option()->set("height", swapped ? boxWidth : boxHeight)->set("size", size)->set("no_rotate", true));
                } else {
                    image = VImage::new_from_file(file.string().c_str(), VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL));

                    if (via == Via::Shrink) {
                        const bool swapped = vips_image_get_orientation_swap(image.get_image()) != 0;
                        const int factor = shrink_factor(image.width(), image.height(), swapped ? boxHeight : boxWidth, swapped ? boxWidth : boxHeight);

                        if (factor > 1) {
                            image = image.shrink(factor, factor);
                        }
                    }
                }

                if (!write_rgba(prepare(image, source, display), out, abort)) {
                    fail(error, file, vips_error());

                    return false;
                }

                return true;
            } catch (const vips::VError &) {
                if (abort != nullptr) {
                    abort->disarm();
                }

                fail(error, file, vips_error());

                return false;
            }
        }

        // The data has to stay mapped until the image is written.
        bool load_vips_buffer(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, Decode::Abort *abort) {
            ensure_vips();

            try {
                if (!write_rgba(prepare(VImage::new_from_buffer(data.data(), data.size(), ""), {}), out, abort)) {
                    fail(error, file, vips_error());

                    return false;
                }

                return true;
            } catch (const vips::VError &) {
                if (abort != nullptr) {
                    abort->disarm();
                }

                fail(error, file, vips_error());

                return false;
            }
        }

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
        bool render_svg_piece(const std::span<const std::uint8_t> document, const double scale, const Size whole, const Part piece, Bitmap *out) {
            const int left = std::max(piece.x - SVG_REACH, 0);
            const int top = std::max(piece.y - SVG_REACH, 0);
            const int width = std::min(piece.x + piece.width + SVG_REACH, whole.width) - left;
            const int height = std::min(piece.y + piece.height + SVG_REACH, whole.height) - top;
            const std::string narrowed = Svg::narrow(document, scale, whole.width, whole.height, left, top, width, height);

            if (narrowed.empty()) {
                return false;
            }

            try {
                const VImage drawn = VImage::new_from_buffer(narrowed.data(), narrowed.size(), "");

                if (drawn.width() != width || drawn.height() != height) {
                    return false;
                }

                return write_rgba(prepare(drawn.crop(piece.x - left, piece.y - top, piece.width, piece.height), {}), out, nullptr);
            } catch (const vips::VError &) {
                return false;
            }
        }

        // The pieces render on threads of their own, since libvips would draw them one by one.
        bool render_svg(const std::span<const std::uint8_t> document, const double scale, const Size whole, const Part part, Bitmap *out, const Decode::Abort *abort) {
            constexpr int STEP = SVG_TILE - (2 * SVG_REACH);
            const int across = (part.width + STEP - 1) / STEP;
            const int down = (part.height + STEP - 1) / STEP;
            const int pieceWidth = (part.width + across - 1) / across;
            const int pieceHeight = (part.height + down - 1) / down;
            const int count = across * down;
            const int wanted = std::min(MAX_VIPS_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
            Bitmap target = Bitmap::allocate(part.width, part.height);
            std::atomic<int> next = 0;
            std::atomic<bool> failed = false;

            const auto work = [&] {
                for (int index = next++; index < count && !failed && !aborted(abort); index = next++) {
                    const int column = index % across;
                    const int row = index / across;
                    const Part piece{part.x + (column * pieceWidth), part.y + (row * pieceHeight), std::min(pieceWidth, part.width - (column * pieceWidth)), std::min(pieceHeight, part.height - (row * pieceHeight))};
                    Bitmap drawn;

                    if (!render_svg_piece(document, scale, whole, piece, &drawn)) {
                        failed = true;

                        break;
                    }

                    for (int y = 0; y < drawn.height(); ++y) {
                        std::ranges::copy(drawn.row(y), target.row((row * pieceHeight) + y).subspan(static_cast<std::size_t>(column * pieceWidth) * Bitmap::CHANNELS).begin());
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

            if (failed || aborted(abort)) {
                return false;
            }

            *out = std::move(target);

            return true;
        }

        // The smallest integer shrink that brings every frame together within the bytes.
        int frame_shrink(const int width, const int height, const int frames, const std::size_t maxBytes) {
            const auto bytes = [&](const int factor) {
                return static_cast<std::size_t>((width + factor - 1) / factor) * static_cast<std::size_t>((height + factor - 1) / factor) * Bitmap::CHANNELS
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

        // --- JPEG ---

        struct JpegError {
            jpeg_error_mgr pub{};
            // NOLINTNEXTLINE(cert-err52-cpp,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): libjpeg reports errors by longjmp only.
            std::jmp_buf jump{};
        };

        [[noreturn]] void jpeg_fail(j_common_ptr info) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): pub is the first member, as libjpeg requires.
            auto *error = reinterpret_cast<JpegError *>(info->err);

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            std::longjmp(error->jump, 1);
        }

        void jpeg_quiet(j_common_ptr /*info*/) {
        }

        struct JpegHandle {
            jpeg_decompress_struct info{};
            JpegError error{};
            bool created = false;

            JpegHandle() {
                info.err = jpeg_std_error(&error.pub);
                error.pub.error_exit = jpeg_fail;
                error.pub.output_message = jpeg_quiet;
            }

            ~JpegHandle() {
                if (created) {
                    jpeg_destroy_decompress(&info);
                }
            }

            JpegHandle(const JpegHandle &) = delete;
            JpegHandle(JpegHandle &&) = delete;
            JpegHandle &operator=(const JpegHandle &) = delete;
            JpegHandle &operator=(JpegHandle &&) = delete;
        };

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

        // Reads the header into handle. False on a malformed file.
        bool jpeg_open(JpegHandle &handle, const std::span<const std::uint8_t> data) {
            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            if (setjmp(handle.error.jump) != 0) {
                return false;
            }

            jpeg_create_decompress(&handle.info);
            handle.created = true;
            jpeg_mem_src(&handle.info, data.data(), static_cast<unsigned long>(data.size()));
            jpeg_save_markers(&handle.info, JPEG_APP0 + 1, 0xFFFF);

            return jpeg_read_header(&handle.info, TRUE) == JPEG_HEADER_OK;
        }

        constexpr unsigned JPEG_DENOM = 8;

        Size jpeg_scaled(const jpeg_decompress_struct &info, const unsigned num) {
            return {static_cast<int>(((static_cast<unsigned long>(info.image_width) * num) + JPEG_DENOM - 1) / JPEG_DENOM),
                    static_cast<int>(((static_cast<unsigned long>(info.image_height) * num) + JPEG_DENOM - 1) / JPEG_DENOM)};
        }

        // Only the powers of two have SIMD inverse transforms in libjpeg-turbo. The others
        // decode slower than the whole image.
        constexpr std::array<unsigned, 4> JPEG_SCALES = {1, 2, 4, 8};

        // The smallest scale that still covers the fitted size, or when forced, the largest
        // that stays within the box.
        unsigned jpeg_scale(const jpeg_decompress_struct &info, const Size box, const int orientation, const Decode::Fit fit) {
            const int boxWidth = Orient::swaps(orientation) ? box.height : box.width;
            const int boxHeight = Orient::swaps(orientation) ? box.width : box.height;

            if (fit == Decode::Fit::Force) {
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

        // Rows are read into the bitmap, which the caller has sized from output_width and
        // output_height. False on a libjpeg error or an abort.
        bool jpeg_read(JpegHandle &handle, Bitmap &target, std::vector<JSAMPROW> &rows, const Decode::Abort *abort) {
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

        bool probe_jpeg(const std::span<const std::uint8_t> data, Decode::Info *info) {
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

        Direct jpeg_decode(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit) {
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
            Size want{boxWidth, boxHeight};

            if (fit != Decode::Fit::Force) {
                const int shownWidth = Orient::swaps(orientation) ? height : width;
                const int shownHeight = Orient::swaps(orientation) ? width : height;

                want = fitted(shownWidth, shownHeight, boxWidth, boxHeight);
            }

            handle.info.scale_num = jpeg_scale(handle.info, want, orientation, fit);
            handle.info.scale_denom = JPEG_DENOM;
            handle.info.out_color_space = JCS_EXT_RGBA;
            handle.info.dct_method = JDCT_ISLOW;

            jpeg_calc_output_dimensions(&handle.info);

            Bitmap held = Bitmap::allocate(static_cast<int>(handle.info.output_width), static_cast<int>(handle.info.output_height));
            std::vector<JSAMPROW> rows(static_cast<std::size_t>(std::max(handle.info.rec_outbuf_height, 1)) + 1);

            if (!jpeg_read(handle, held, rows, abort)) {
                fail(error, file, aborted(abort) ? "aborted" : "jpeg decode failed");

                return Direct::Failed;
            }

            *out = std::move(held);

            return Direct::Done;
        }

        // Bands of rows of the height on up to MAX_VIPS_THREADS threads, until an abort.
        void parallel_rows(const int height, const std::function<void(int from, int to)> &each, const Decode::Abort *abort) {
            const int bands = (height + ABORT_ROWS - 1) / ABORT_ROWS;
            const int wanted = std::min(MAX_VIPS_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
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
        bool jpeg_lift(const std::span<const std::uint8_t> data, const Tone::Display &display, Bitmap *base, const Decode::Abort *abort) {
            GainMap::Jpeg gain;
            Bitmap map;

            if (!GainMap::find_jpeg(data, &gain)) {
                return false;
            }

            const float weight = gain.metadata.weight(std::log2(display.headroom));
            constexpr int WHOLE = std::numeric_limits<int>::max();

            if (weight == 0.0F || jpeg_decode("gain map", gain.image, WHOLE, WHOLE, &map, nullptr, abort, Decode::Fit::Cheap) != Direct::Done) {
                return false;
            }

            const GainMap::Applier applier(gain.metadata, &map, base->width(), base->height(), weight);
            const Tone::Mapper mapper({}, display);

            parallel_rows(base->height(), [&](const int from, const int to) {
                for (int y = from; y < to; ++y) {
                    const std::span<std::uint8_t> row = base->row(y);

                    mapper.map(std::span<const std::uint8_t>(row), Bitmap::CHANNELS, row, [&applier, y](const std::size_t first, const std::span<float> rgba) {
                        applier.apply(static_cast<int>(first), y, rgba);
                    });
                }
            }, abort);

            base->set_encoding(Bitmap::Encoding::Pq);

            return !aborted(abort);
        }

        // The base rendition, which an HDR display shows lifted by the gain map if there is one.
        Direct load_jpeg(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort,
                         const Decode::Fit fit, const Tone::Display &display) {
            const Direct direct = jpeg_decode(file, data, boxWidth, boxHeight, out, error, abort, fit);

            if (direct == Direct::Done && display.hdr() && !jpeg_lift(data, display, out, abort) && aborted(abort)) {
                *out = {};
                fail(error, file, "aborted");

                return Direct::Failed;
            }

            return direct;
        }

        // --- PNG ---

        struct PngReader {
            std::span<const std::uint8_t> data;
            std::size_t at = 0;
        };

        void png_pull(png_structp png, png_bytep out, const png_size_t count) {
            auto *reader = static_cast<PngReader *>(png_get_io_ptr(png));

            if (reader->at + count > reader->data.size()) {
                png_error(png, "truncated");
            }

            std::memcpy(out, reader->data.subspan(reader->at, count).data(), count);
            reader->at += count;
        }

        void png_fail(png_structp png, png_const_charp /*message*/) {
            png_longjmp(png, 1);
        }

        void png_quiet(png_structp /*png*/, png_const_charp /*message*/) {
        }

        struct PngHandle {
            png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, png_fail, png_quiet);
            png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;

            PngHandle() = default;

            ~PngHandle() {
                png_destroy_read_struct(&png, &info, nullptr);
            }

            PngHandle(const PngHandle &) = delete;
            PngHandle(PngHandle &&) = delete;
            PngHandle &operator=(const PngHandle &) = delete;
            PngHandle &operator=(PngHandle &&) = delete;
        };

        int png_orientation(const PngHandle &handle) {
#ifdef PNG_eXIf_SUPPORTED
            png_uint_32 length = 0;
            png_bytep exif = nullptr;

            if (png_get_eXIf_1(handle.png, handle.info, &length, &exif) != 0 && exif != nullptr) {
                return Exif::orientation({exif, length});
            }
#endif

            return 1;
        }

        // Reads the header and sets the transforms that make every PNG come out RGBA8, or
        // RGBA of 16 bits in native order when wide, for the tone mapper.
        bool png_open(PngHandle &handle, PngReader &reader, const bool wide) {
            if (handle.png == nullptr || handle.info == nullptr) {
                return false;
            }

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp): libpng reports errors by longjmp only.
            if (setjmp(png_jmpbuf(handle.png)) != 0) {
                return false;
            }

            png_set_read_fn(handle.png, &reader, png_pull);
            png_read_info(handle.png, handle.info);

            const png_byte colour = png_get_color_type(handle.png, handle.info);
            const png_byte depth = png_get_bit_depth(handle.png, handle.info);

            png_set_expand(handle.png);

            if (wide) {
                png_set_expand_16(handle.png);

                if constexpr (std::endian::native == std::endian::little) {
                    png_set_swap(handle.png);
                }
            } else if (depth == 16) {
                png_set_scale_16(handle.png);
            }

            if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) {
                png_set_gray_to_rgb(handle.png);
            }

            if ((colour & PNG_COLOR_MASK_ALPHA) == 0 && png_get_valid(handle.png, handle.info, PNG_INFO_tRNS) == 0) {
                png_set_filler(handle.png, wide ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
            }

            return true;
        }

        // Rows come out RGBA8, through the tone mapper when png_open made them wide. Wide rows
        // gather in bands that a thread of its own maps while libpng reads the next, since the
        // mapping would add a tenth to the decode.
        class PngRows {

        public:
            // Hears each row in order, where rows have nowhere else to go. Mapped rows come from the mapping thread.
            using Landed = std::function<void(std::span<const std::uint8_t> row)>;

            PngRows(const Tone::Mapper *mapper, const int width, Landed landed = {})
                : _mapper(mapper), _pitch(static_cast<std::size_t>(width) * Bitmap::CHANNELS), _landed(std::move(landed)) {
                if (_mapper == nullptr) {
                    return;
                }

                _rows = static_cast<int>(std::clamp<std::size_t>(PNG_BAND_BYTES / (_pitch * sizeof(std::uint16_t)), 1, ABORT_ROWS));

                for (Band &band : _bands) {
                    band.wide.resize(_pitch * static_cast<std::size_t>(_rows));
                    band.out.resize(static_cast<std::size_t>(_rows));
                }

                if (_landed) {
                    _scratch.resize(_pitch);
                }

                _worker = std::jthread([this] { work(); });
            }

            PngRows(const PngRows &) = delete;
            PngRows &operator=(const PngRows &) = delete;
            PngRows(PngRows &&) = delete;
            PngRows &operator=(PngRows &&) = delete;

            ~PngRows() { finish(); }

            [[nodiscard]] std::size_t bytes() const { return _pitch * (_mapper != nullptr ? sizeof(std::uint16_t) : 1); }

            // Into out, or through out to landed when there is one. Mapped rows arrive once their
            // band is done, which finish() waits for.
            void read(png_structp png, const std::span<std::uint8_t> out) {
                if (_mapper == nullptr) {
                    png_read_row(png, out.data(), nullptr);

                    if (_landed) {
                        _landed(out);
                    }

                    return;
                }

                Band &band = _bands.at(_filling);

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libpng writes the 16 bit samples as bytes.
                png_read_row(png, reinterpret_cast<png_bytep>(std::span(band.wide).subspan(_pitch * static_cast<std::size_t>(band.rows), _pitch).data()), nullptr);
                band.out.at(static_cast<std::size_t>(band.rows)) = out;

                if (++band.rows == _rows) {
                    hand_over();
                }
            }

            void finish() {
                if (!_worker.joinable()) {
                    return;
                }

                if (_bands.at(_filling).rows > 0) {
                    hand_over();
                }

                {
                    const std::scoped_lock hold(_guard);

                    _closing = true;
                }

                _moved.notify_all();
                _worker.join();
            }

        private:
            struct Band {
                std::vector<std::uint16_t> wide;
                std::vector<std::span<std::uint8_t>> out;
                int rows = 0;
                bool full = false;
            };

            void hand_over() {
                {
                    const std::scoped_lock hold(_guard);

                    _bands.at(_filling).full = true;
                }

                _moved.notify_all();
                _filling ^= 1U;

                std::unique_lock hold(_guard);

                _moved.wait(hold, [this] { return !_bands.at(_filling).full; });
            }

            // Takes the bands in the order they fill, so a band not yet full once closing is the last.
            void work() {
                for (std::size_t slot = 0;; slot ^= 1U) {
                    Band &band = _bands.at(slot);

                    {
                        std::unique_lock hold(_guard);

                        _moved.wait(hold, [&] { return band.full || _closing; });

                        if (!band.full) {
                            return;
                        }
                    }

                    for (int row = 0; row < band.rows; ++row) {
                        const auto at = static_cast<std::size_t>(row);
                        const std::span<std::uint8_t> out = _landed ? std::span(_scratch) : band.out.at(at);

                        _mapper->map(std::span<const std::uint16_t>(band.wide).subspan(_pitch * at, _pitch), Bitmap::CHANNELS, out);

                        if (_landed) {
                            _landed(out);
                        }
                    }

                    {
                        const std::scoped_lock hold(_guard);

                        band.rows = 0;
                        band.full = false;
                    }

                    _moved.notify_all();
                }
            }

            const Tone::Mapper *_mapper;
            std::size_t _pitch;
            Landed _landed;
            int _rows = 0;
            std::array<Band, 2> _bands;
            std::size_t _filling = 0;
            std::vector<std::uint8_t> _scratch;
            std::mutex _guard;
            std::condition_variable _moved;
            bool _closing = false;
            std::jthread _worker;
        };

        // Wide rows only for images that are not interlaced, whose passes build on the rows before.
        bool png_read(PngHandle &handle, Bitmap &target, const Tone::Mapper *mapper, const Decode::Abort *abort) {
            PngRows rows(mapper, target.width());

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp): libpng reports errors by longjmp only.
            if (setjmp(png_jmpbuf(handle.png)) != 0) {
                return false;
            }

            const int passes = png_set_interlace_handling(handle.png);

            png_read_update_info(handle.png, handle.info);

            if (png_get_rowbytes(handle.png, handle.info) != rows.bytes() || png_get_channels(handle.png, handle.info) != Bitmap::CHANNELS) {
                return false;
            }

            for (int pass = 0; pass < passes; ++pass) {
                for (int y = 0; y < target.height(); ++y) {
                    if (y % ABORT_ROWS == 0 && aborted(abort)) {
                        return false;
                    }

                    rows.read(handle.png, target.row(y));
                }
            }

            png_read_end(handle.png, nullptr);
            rows.finish();

            return true;
        }

        // Rows stream through a box filter into the target, so an image of any size costs its
        // shrunk size plus two rows. Only for images that are not interlaced.
        bool png_read_shrunk(PngHandle &handle, Bitmap &target, const int factor, const Tone::Mapper *mapper, const Decode::Abort *abort) {
            const auto width = static_cast<int>(png_get_image_width(handle.png, handle.info));
            const auto height = static_cast<int>(png_get_image_height(handle.png, handle.info));
            std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * Bitmap::CHANNELS);
            BoxShrink shrink(width, height, factor, &target);
            PngRows rows(mapper, width, [&shrink](const std::span<const std::uint8_t> mapped) { shrink.push(mapped); });

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp): libpng reports errors by longjmp only.
            if (setjmp(png_jmpbuf(handle.png)) != 0) {
                return false;
            }

            png_read_update_info(handle.png, handle.info);

            if (png_get_rowbytes(handle.png, handle.info) != rows.bytes() || png_get_channels(handle.png, handle.info) != Bitmap::CHANNELS) {
                return false;
            }

            for (int y = 0; y < height; ++y) {
                if (y % ABORT_ROWS == 0 && aborted(abort)) {
                    return false;
                }

                rows.read(handle.png, row);
            }

            png_read_end(handle.png, nullptr);
            rows.finish();

            return true;
        }

        std::uint32_t png_u32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return (static_cast<std::uint32_t>(data[at]) << 24) | (static_cast<std::uint32_t>(data[at + 1]) << 16) | (static_cast<std::uint32_t>(data[at + 2]) << 8) | data[at + 3];
        }

        // A chunk that comes before the image data, as eXIf and cICP do:
        // https://www.w3.org/TR/png-3/#5ChunkOrdering
        std::span<const std::uint8_t> png_chunk(const std::span<const std::uint8_t> data, const std::string_view type) {
            for (std::size_t at = 8; at + 12 <= data.size() && !starts_with(data, "IDAT", at + 4);) {
                const std::size_t length = png_u32(data, at);

                if (length > data.size() - at - 12) {
                    break;
                }

                if (starts_with(data, type, at + 4)) {
                    return data.subspan(at + 8, length);
                }

                at += 12 + length;
            }

            return {};
        }

        // https://www.w3.org/TR/png-3/#cICP-chunk
        Tone::Source png_tone(const std::span<const std::uint8_t> data) {
            const std::span<const std::uint8_t> cicp = png_chunk(data, "cICP");

            return cicp.size() >= 2 ? Tone::from_cicp(cicp[0], cicp[1]) : Tone::Source{};
        }

        bool probe_png(const std::span<const std::uint8_t> data, Decode::Info *info) {
            // Width and height sit at fixed offsets in IHDR, the first chunk: https://www.w3.org/TR/png-3/#11IHDR
            constexpr std::size_t IHDR = 16;

            if (data.size() < IHDR + 8) {
                return false;
            }

            const auto width = static_cast<int>(png_u32(data, IHDR));
            const auto height = static_cast<int>(png_u32(data, IHDR + 4));

            if (const std::span<const std::uint8_t> exif = png_chunk(data, "eXIf"); !exif.empty()) {
                info->orientation = Exif::orientation(exif);
            }

            info->width = Orient::swaps(info->orientation) ? height : width;
            info->height = Orient::swaps(info->orientation) ? width : height;
            info->hdr = png_tone(data).hdr();

            return width > 0 && height > 0;
        }

        Direct load_png(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit,
                        const Tone::Display &display = {}) {
            PngHandle handle;
            PngReader reader{data, 0};
            const Tone::Source tone = png_tone(data);

            if (!png_open(handle, reader, tone.hdr())) {
                fail(error, file, "png header unreadable");

                return Direct::Failed;
            }

            const int orientation = png_orientation(handle);
            const auto width = static_cast<int>(png_get_image_width(handle.png, handle.info));
            const auto height = static_cast<int>(png_get_image_height(handle.png, handle.info));
            const int factor = fit == Decode::Fit::Force ? shrink_factor(width, height, Orient::swaps(orientation) ? boxHeight : boxWidth, Orient::swaps(orientation) ? boxWidth : boxHeight) : 1;

            if ((factor > 1 || tone.hdr()) && png_get_interlace_type(handle.png, handle.info) != PNG_INTERLACE_NONE) {
                return Direct::Skip;
            }

            const std::optional<Tone::Mapper> mapper = tone.hdr() ? std::optional(Tone::Mapper(tone, display)) : std::nullopt;
            const Tone::Mapper *mapping = mapper ? &*mapper : nullptr;
            const Bitmap::Encoding encoding = tone.hdr() && display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;
            Bitmap held = Bitmap::allocate((width + factor - 1) / factor, (height + factor - 1) / factor, encoding);
            const bool read = factor > 1 ? png_read_shrunk(handle, held, factor, mapping, abort) : png_read(handle, held, mapping, abort);

            if (!read) {
                fail(error, file, aborted(abort) ? "aborted" : "png decode failed");

                return Direct::Failed;
            }

            *out = std::move(held);

            return Direct::Done;
        }

        // --- WebP ---

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

        // An animation is left to libvips, which counts its frames.
        bool probe_webp(const std::span<const std::uint8_t> data, Decode::Info *info) {
            WebPBitstreamFeatures features;

            if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK || features.has_animation != 0) {
                return false;
            }

            info->orientation = webp_orientation(data);
            info->width = Orient::swaps(info->orientation) ? features.height : features.width;
            info->height = Orient::swaps(info->orientation) ? features.width : features.height;

            return true;
        }

        // Straight into the bitmap at native size. Scaling in libwebp measured slower than a
        // full decode followed by halving, so the box is not used. Animation stays with libvips.
        Direct load_webp(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, const Decode::Abort *abort) {
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

        // --- JPEG XL ---

        struct JxlHandle {
            JxlDecoder *decoder = JxlDecoderCreate(nullptr);
            void *runner = JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());

            JxlHandle() = default;

            ~JxlHandle() {
                JxlThreadParallelRunnerDestroy(runner);
                JxlDecoderDestroy(decoder);
            }

            JxlHandle(const JxlHandle &) = delete;
            JxlHandle(JxlHandle &&) = delete;
            JxlHandle &operator=(const JxlHandle &) = delete;
            JxlHandle &operator=(JxlHandle &&) = delete;
        };

        Size jxl_size(const JxlBasicInfo &info) {
            const bool swapped = info.orientation >= JXL_ORIENT_TRANSPOSE;

            return {static_cast<int>(swapped ? info.ysize : info.xsize), static_cast<int>(swapped ? info.xsize : info.ysize)};
        }

        // The encoding the pixels come out in. An ICC profile alone is taken for SDR.
        Tone::Source jxl_tone(JxlDecoder *decoder) {
            JxlColorEncoding encoding;

            if (JxlDecoderGetColorAsEncodedProfile(decoder, JXL_COLOR_PROFILE_TARGET_DATA, &encoding) != JXL_DEC_SUCCESS) {
                return {};
            }

            // The enumerations take their values from H.273.
            return Tone::from_cicp(static_cast<int>(encoding.primaries), static_cast<int>(encoding.transfer_function));
        }

        // Up to the colour encoding: the size and whether it is HDR.
        bool jxl_header(const std::span<const std::uint8_t> data, JxlBasicInfo *basic, Tone::Source *tone) {
            const JxlHandle handle;

            if (handle.decoder == nullptr || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return false;
            }

            JxlDecoderCloseInput(handle.decoder);

            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    if (JxlDecoderGetBasicInfo(handle.decoder, basic) != JXL_DEC_SUCCESS) {
                        return false;
                    }
                } else if (status == JXL_DEC_COLOR_ENCODING) {
                    *tone = jxl_tone(handle.decoder);

                    return true;
                } else {
                    return false;
                }
            }
        }

        Tone::Source jxl_container_tone(const std::span<const std::uint8_t> data) {
            JxlBasicInfo basic;
            Tone::Source tone;

            return jxl_header(data, &basic, &tone) ? tone : Tone::Source{};
        }

        bool probe_jxl(const std::span<const std::uint8_t> data, Decode::Info *info) {
            JxlBasicInfo basic;
            Tone::Source tone;

            if (!jxl_header(data, &basic, &tone)) {
                return false;
            }

            const Size size = jxl_size(basic);

            info->width = size.width;
            info->height = size.height;
            info->orientation = static_cast<int>(basic.orientation);
            info->hdr = tone.hdr() || !Heif::box(data, "jhgm").empty();

            return true;
        }

        // A gain map that brings an HDR base down to its SDR rendition, as a jhgm box carries it.
        // Gains outside the base's colours are in sRGB's, the only ones read.
        struct JxlGain {
            GainMap::Metadata metadata;
            Bitmap map;
        };

        // Where a run of pixels libjxl hands over goes, and what hears it has landed.
        using JxlPlace = std::function<std::span<std::uint8_t>(std::size_t x, std::size_t y, std::size_t pixels)>;
        using JxlLanded = std::function<void(std::size_t y, std::size_t pixels)>;

        // How libjxl's pixels become four bytes a pixel, a run of a row at a time from several
        // threads: copied as RGBA8, or through the tone mapper and a gain map.
        struct JxlOutput {
            std::optional<Tone::Mapper> mapper;
            JxlDataType type = JXL_TYPE_UINT8;
            Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
            // Held apart, as the applier keeps the map's address.
            std::unique_ptr<JxlGain> gainMap;
            std::optional<GainMap::Applier> gain;
            // Into the colour space the gains apply in and back, when that is not the base's own.
            std::optional<std::pair<Tone::Matrix, Tone::Matrix>> gainColours;
            JxlPlace place;
            JxlLanded landed;

            void take(const std::size_t x, const std::size_t y, const std::size_t pixels, const void *samples) const {
                const std::span<std::uint8_t> out = place(x, y, pixels);
                const std::size_t count = pixels * Bitmap::CHANNELS;
                // Captured by reference, so the adjust fits in std::function without an allocation per run.
                const std::array<std::size_t, 2> start{x, y};
                const Tone::Adjust adjust = gain ? Tone::Adjust([this, &start](const std::size_t first, const std::span<float> rgba) { lift(start, first, rgba); }) : Tone::Adjust{};

                if (!mapper) {
                    std::memcpy(out.data(), samples, count);
                } else if (type == JXL_TYPE_FLOAT) {
                    mapper->map(std::span(static_cast<const float *>(samples), count), Bitmap::CHANNELS, out, adjust);
                } else {
                    mapper->map(std::span(static_cast<const std::uint16_t *>(samples), count), Bitmap::CHANNELS, out, adjust);
                }

                if (landed) {
                    landed(y, pixels);
                }
            }

            void lift(const std::array<std::size_t, 2> &start, const std::size_t first, const std::span<float> rgba) const {
                const auto [x, y] = start;

                if (!gain) {
                    return;
                }

                if (gainColours) {
                    Tone::transform(gainColours->first, rgba);
                }

                gain->apply(static_cast<int>(x + first), static_cast<int>(y), rgba);

                if (gainColours) {
                    Tone::transform(gainColours->second, rgba);
                }
            }
        };

        void jxl_take(void *opaque, const std::size_t x, const std::size_t y, const std::size_t pixels, const void *samples) {
            static_cast<const JxlOutput *>(opaque)->take(x, y, pixels, samples);
        }

        // Told the colour encoding, says how the pixels come out, or nothing for RGBA8 as stored.
        using JxlSetup = std::function<std::optional<JxlOutput>(const Tone::Source &tone, int width, int height)>;

        // RGBA8 straight into the bitmap, or through the output's callback.
        bool jxl_output(JxlDecoder *decoder, Bitmap &held, JxlOutput *output) {
            if (output != nullptr) {
                const JxlPixelFormat format{Bitmap::CHANNELS, output->type, JXL_NATIVE_ENDIAN, 0};

                return JxlDecoderSetImageOutCallback(decoder, &format, jxl_take, output) == JXL_DEC_SUCCESS;
            }

            const JxlPixelFormat format{Bitmap::CHANNELS, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
            std::size_t needed = 0;

            return JxlDecoderImageOutBufferSize(decoder, &format, &needed) == JXL_DEC_SUCCESS && needed == held.bytes()
                   && JxlDecoderSetImageOutBuffer(decoder, &format, held.data(), held.bytes()) == JXL_DEC_SUCCESS;
        }

        // The bitmap sized for the image, false for an animation.
        bool jxl_canvas(JxlDecoder *decoder, Bitmap *held) {
            JxlBasicInfo info;

            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS || info.have_animation != 0) {
                return false;
            }

            *held = Bitmap::allocate(static_cast<int>(info.xsize), static_cast<int>(info.ysize));

            return true;
        }

        // An output that maps writes into the bitmap, reallocated in the output's encoding.
        void jxl_bind(std::optional<JxlOutput> &output, Bitmap *held) {
            if (!output) {
                return;
            }

            *held = Bitmap::allocate(held->width(), held->height(), output->encoding);
            output->place = [held](const std::size_t x, const std::size_t y, const std::size_t pixels) {
                return held->row(static_cast<int>(y)).subspan(x * Bitmap::CHANNELS, pixels * Bitmap::CHANNELS);
            };
        }

        // Without a setup the samples come out as stored, which a gain map wants. libjxl runs
        // with its own thread pool, which libvips does not use.
        Direct jxl_decode(const std::span<const std::uint8_t> data, Bitmap *out, const Decode::Abort *abort, const JxlSetup &setup) {
            const JxlHandle handle;
            const int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | (setup ? JXL_DEC_COLOR_ENCODING : 0);

            if (handle.decoder == nullptr || handle.runner == nullptr
                || JxlDecoderSetParallelRunner(handle.decoder, JxlThreadParallelRunner, handle.runner) != JXL_DEC_SUCCESS
                || JxlDecoderSetKeepOrientation(handle.decoder, JXL_TRUE) != JXL_DEC_SUCCESS
                || JxlDecoderSubscribeEvents(handle.decoder, events) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return Direct::Skip;
            }

            JxlDecoderCloseInput(handle.decoder);

            Bitmap held;
            std::optional<JxlOutput> output;

            for (;;) {
                if (aborted(abort)) {
                    return Direct::Failed;
                }

                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    if (!jxl_canvas(handle.decoder, &held)) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_COLOR_ENCODING) {
                    // Basic info comes first, so the bitmap is there.
                    output = setup(jxl_tone(handle.decoder), held.width(), held.height());
                    jxl_bind(output, &held);
                } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    if (held.empty() || !jxl_output(handle.decoder, held, output ? &*output : nullptr)) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
                    break;
                } else {
                    return Direct::Failed;
                }
            }

            if (held.empty()) {
                return Direct::Skip;
            }

            *out = std::move(held);

            return Direct::Done;
        }

        std::unique_ptr<JxlGain> jxl_gain(const std::span<const std::uint8_t> data) {
            GainMap::Jxl bundle;
            auto held = std::make_unique<JxlGain>();

            if (!GainMap::read_jxl(Heif::box(data, "jhgm"), &bundle) || jxl_decode(bundle.image, &held->map, nullptr, {}) != Direct::Done) {
                return nullptr;
            }

            held->metadata = bundle.metadata;

            return held;
        }

        // HDR is tone mapped, or brought down to SDR by a gain map where there is one.
        std::optional<JxlOutput> jxl_setup(const std::span<const std::uint8_t> data, const Tone::Source &tone, const int width, const int height, const Tone::Display &display) {
            if (!tone.hdr()) {
                return std::nullopt;
            }

            std::unique_ptr<JxlGain> gain = jxl_gain(data);
            const float weight = gain != nullptr ? gain->metadata.weight(std::log2(display.headroom)) : 0.0F;
            // Brought down to SDR, the gain map leaves nothing to roll off, and clipping keeps its colours.
            const bool rolledOff = weight == 0.0F || display.hdr();
            JxlOutput held;

            held.mapper.emplace(tone, display, rolledOff);
            held.type = tone.transfer == Tone::Transfer::Linear ? JXL_TYPE_FLOAT : JXL_TYPE_UINT16;
            held.encoding = display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;

            if (gain != nullptr && weight != 0.0F) {
                held.gain.emplace(gain->metadata, &gain->map, width, height, weight);

                if (!gain->metadata.inBaseColours && tone.primaries != Tone::Primaries::Bt709) {
                    held.gainColours.emplace(Tone::convert(tone.primaries, Tone::Primaries::Bt709), Tone::convert(Tone::Primaries::Bt709, tone.primaries));
                }

                held.gainMap = std::move(gain);
            }

            return held;
        }

        // Runs libjxl hands over from many threads in no order, gathered into bands of rows that
        // go on in order. A thread more than a few bands ahead of the one going on waits, so the
        // bands in memory stay few while the decode outruns whatever takes them. The runs of the
        // bands before it were claimed first by other threads, so the wait always ends.
        class JxlBands {

        public:
            static constexpr std::size_t AHEAD = 4;

            JxlBands(const int width, const int height, const int rows, const Bitmap::Encoding encoding, const Decode::Take &take, const Decode::Abort *abort)
                : _width(width), _height(height), _rows(rows), _encoding(encoding), _take(take), _abort(abort), _slots(static_cast<std::size_t>((height + rows - 1) / rows)) {
            }

            std::span<std::uint8_t> place(const std::size_t x, const std::size_t y, const std::size_t pixels) {
                const std::size_t index = y / static_cast<std::size_t>(_rows);
                Slot &slot = _slots.at(index);

                wait_for(index);
                std::call_once(slot.made, [&] {
                    const int rows = std::min(_rows, _height - (static_cast<int>(index) * _rows));

                    slot.band = Bitmap::allocate(_width, rows, _encoding);
                    slot.remaining.store(static_cast<std::size_t>(_width) * static_cast<std::size_t>(rows));
                });

                return slot.band.row(static_cast<int>(y % static_cast<std::size_t>(_rows))).subspan(x * Bitmap::CHANNELS, pixels * Bitmap::CHANNELS);
            }

            void landed(const std::size_t y, const std::size_t pixels) {
                Slot &slot = _slots.at(y / static_cast<std::size_t>(_rows));

                if (slot.remaining.fetch_sub(pixels) == pixels) {
                    slot.whole.store(true);
                    deliver();
                }
            }

            // Take said no, or the abort came, so the decode should end.
            [[nodiscard]] bool stopped(const Decode::Abort *abort) const { return _failed.load() || aborted(abort); }

            void stop() {
                _failed.store(true);
                _moved.notify_all();
            }

            [[nodiscard]] bool complete() const { return _next == _slots.size() && !_failed.load(); }

        private:
            struct Slot {
                std::once_flag made;
                Bitmap band;
                std::atomic<std::size_t> remaining = 0;
                std::atomic<bool> whole = false;
            };

            // An abort sends no notice, so the wait looks for one now and then. Nearly every run
            // is within reach, which the lock-free look sees.
            void wait_for(const std::size_t index) {
                constexpr auto LOOK = std::chrono::milliseconds(20);
                const auto near = [&] { return index < _next.load() + AHEAD || _failed.load(); };

                if (near()) {
                    return;
                }

                std::unique_lock hold(_guard);

                while (!_moved.wait_for(hold, LOOK, near)) {
                    if (aborted(_abort)) {
                        return;
                    }
                }
            }

            void deliver() {
                const std::scoped_lock hold(_guard);

                while (_next < _slots.size() && _slots.at(_next).whole.load() && !_failed.load()) {
                    Slot &slot = _slots.at(_next);

                    if (!_take(static_cast<int>(_next) * _rows, slot.band.height(), slot.band.all())) {
                        _failed.store(true);
                    }

                    slot.band = {};
                    ++_next;
                    _moved.notify_all();
                }
            }

            int _width;
            int _height;
            int _rows;
            Bitmap::Encoding _encoding;
            const Decode::Take &_take;
            const Decode::Abort *_abort;
            std::vector<Slot> _slots;
            std::mutex _guard;
            std::condition_variable _moved;
            std::atomic<std::size_t> _next = 0;
            std::atomic<bool> _failed = false;
        };

        // libjxl's pool, which stops handing out work once the decode should end, so an abort
        // lands within a group rather than after the whole frame.
        struct JxlStoppable {
            void *pool = nullptr;
            std::function<bool()> stopped;
        };

        struct JxlTask {
            const JxlStoppable *runner = nullptr;
            void *opaque = nullptr;
            JxlParallelRunInit init = nullptr;
            JxlParallelRunFunction run = nullptr;
        };

        JxlParallelRetCode jxl_run(void *opaque, void *jpegxlOpaque, JxlParallelRunInit init, JxlParallelRunFunction run, const std::uint32_t start, const std::uint32_t end) {
            const auto *runner = static_cast<const JxlStoppable *>(opaque);

            if (runner->stopped()) {
                return JXL_PARALLEL_RET_RUNNER_ERROR;
            }

            JxlTask task{runner, jpegxlOpaque, init, run};

            const JxlParallelRetCode code = JxlThreadParallelRunner(runner->pool, &task,
                [](void *held, const std::size_t threads) {
                    const auto *given = static_cast<const JxlTask *>(held);

                    return given->init(given->opaque, threads);
                },
                [](void *held, const std::uint32_t value, const std::size_t thread) {
                    const auto *given = static_cast<const JxlTask *>(held);

                    if (!given->runner->stopped()) {
                        given->run(given->opaque, value, thread);
                    }
                },
                start, end);

            return runner->stopped() ? JXL_PARALLEL_RET_RUNNER_ERROR : code;
        }

        struct JxlStreamTo {
            int rows = 0;
            const Decode::Begin *begin = nullptr;
            const Decode::Take *take = nullptr;
            const Decode::Abort *abort = nullptr;
        };

        // Tells begin what is coming, and points the output at bands that go on to take. An
        // image with nothing to map is copied as it comes.
        bool jxl_start(JxlDecoder *decoder, const std::span<const std::uint8_t> data, const Tone::Display &display, const JxlStreamTo &to, std::optional<JxlOutput> &output,
                       std::optional<JxlBands> &bands) {
            JxlBasicInfo info;

            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
                return false;
            }

            const auto width = static_cast<int>(info.xsize);
            const auto height = static_cast<int>(info.ysize);

            output = jxl_setup(data, jxl_tone(decoder), width, height, display);

            if (!output) {
                output.emplace();
            }

            if (!(*to.begin)(width, height, info.alpha_bits > 0, output->encoding)) {
                return false;
            }

            bands.emplace(width, height, to.rows, output->encoding, *to.take, to.abort);
            output->place = [&bands](const std::size_t x, const std::size_t y, const std::size_t pixels) { return bands->place(x, y, pixels); };
            output->landed = [&bands](const std::size_t y, const std::size_t pixels) { bands->landed(y, pixels); };

            return true;
        }

        bool jxl_animated(JxlDecoder *decoder) {
            JxlBasicInfo info;

            return JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS || info.have_animation != 0;
        }

        bool jxl_listen(JxlDecoder *decoder, std::optional<JxlOutput> &output) {
            if (!output) {
                return false;
            }

            const JxlPixelFormat format{Bitmap::CHANNELS, output->type, JXL_NATIVE_ENDIAN, 0};

            return JxlDecoderSetImageOutCallback(decoder, &format, jxl_take, &*output) == JXL_DEC_SUCCESS;
        }

        void jxl_stop(std::optional<JxlBands> &bands) {
            if (bands) {
                bands->stop();
            }
        }

        // Band by band into take, in however many threads libjxl has, with only a few bands
        // held. Animation stays with libvips.
        Direct jxl_stream(const std::span<const std::uint8_t> data, const int rows, const Decode::Begin &begin, const Decode::Take &take, const Decode::Abort *abort, const Tone::Display &display) {
            const JxlHandle handle;
            std::optional<JxlBands> bands;
            JxlStoppable runner{handle.runner, [&] { return bands ? bands->stopped(abort) : aborted(abort); }};

            if (handle.decoder == nullptr || handle.runner == nullptr || JxlDecoderSetParallelRunner(handle.decoder, jxl_run, &runner) != JXL_DEC_SUCCESS
                || JxlDecoderSetKeepOrientation(handle.decoder, JXL_TRUE) != JXL_DEC_SUCCESS
                || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return Direct::Skip;
            }

            JxlDecoderCloseInput(handle.decoder);

            const JxlStreamTo to{rows, &begin, &take, abort};
            std::optional<JxlOutput> output;
            // Begin is heard at the colour encoding, and from then on there is no going back to libvips.
            Direct failed = Direct::Skip;

            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    if (jxl_animated(handle.decoder)) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_COLOR_ENCODING) {
                    failed = Direct::Failed;

                    if (!jxl_start(handle.decoder, data, display, to, output, bands)) {
                        return Direct::Failed;
                    }
                } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    if (!jxl_listen(handle.decoder, output)) {
                        return Direct::Failed;
                    }
                } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
                    break;
                } else {
                    jxl_stop(bands);

                    return failed;
                }
            }

            return bands && bands->complete() ? Direct::Done : Direct::Failed;
        }

        // Animation stays with libvips.
        Direct load_jxl(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, const Decode::Abort *abort, const Tone::Display &display) {
            const Direct direct = jxl_decode(data, out, abort, [data, display](const Tone::Source &tone, const int width, const int height) {
                return jxl_setup(data, tone, width, height, display);
            });

            if (direct == Direct::Failed) {
                fail(error, file, aborted(abort) ? "aborted" : "jxl decode failed");
            }

            return direct;
        }

        // libjxl keeps float rows along every group border of a lossy frame, for the filters that
        // run across groups, which comes to about 2.7 bytes a pixel in measurements. Lossless
        // frames need far less.
        constexpr double JXL_LOSSY_BYTES = 2.8;
        constexpr double JXL_LOSSLESS_BYTES = 1.0;

        // The transfer of an HDR file that libvips reads without it.
        Tone::Source container_tone(const Decode::Format kind, const std::span<const std::uint8_t> data) {
            switch (kind) {
                case Decode::Format::Png:
                    return png_tone(data);
                case Decode::Format::Jxl:
                    return jxl_container_tone(data);
                case Decode::Format::Heif:
                    return Heif::colour(data).tone;
                default:
                    break;
            }

            return {};
        }

        Tone::Source container_tone(const std::filesystem::path &file) {
            Mapped mapped;

            return Mapped::open(file, &mapped) ? container_tone(Decode::sniff(mapped.data()), mapped.data()) : Tone::Source{};
        }

        // --- BMP ---

        bool probe_bmp(const std::span<const std::uint8_t> data, Decode::Info *info) {
            Bmp::Image image;

            if (!Bmp::Image::open(data, &image)) {
                return false;
            }

            info->width = image.width();
            info->height = image.height();

            return true;
        }

        // A variant the decoder does not know, such as an embedded JPEG, is left to libvips.
        Direct load_bmp(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit) {
            Bmp::Image image;

            if (!Bmp::Image::open(data, &image)) {
                return Direct::Skip;
            }

            const int factor = fit == Decode::Fit::Force ? shrink_factor(image.width(), image.height(), boxWidth, boxHeight) : 1;

            if (!image.decode(factor, out, abort)) {
                fail(error, file, aborted(abort) ? "aborted" : "bmp decode failed");

                return Direct::Failed;
            }

            return Direct::Done;
        }

        // --- ICO and ICNS ---

        bool icon_entry(const Decode::Format kind, const std::span<const std::uint8_t> data, Icon::Entry *entry) {
            return kind == Decode::Format::Ico ? Icon::largest_ico(data, entry) : Icon::largest_icns(data, entry);
        }

        bool probe_icon(const Decode::Format kind, const std::span<const std::uint8_t> data, Decode::Info *info) {
            Icon::Entry entry;

            if (!icon_entry(kind, data, &entry)) {
                return false;
            }

            info->width = entry.width;
            info->height = entry.height;

            return true;
        }

        // Only the largest entry is shown.
        Direct load_icon(const std::filesystem::path &file, const Decode::Format kind, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Decode::Abort *abort, const Decode::Fit fit) {
            Icon::Entry entry;

            if (!icon_entry(kind, data, &entry)) {
                return Direct::Skip;
            }

            switch (entry.payload) {
                case Icon::Payload::Png:
                    return load_png(file, entry.data, boxWidth, boxHeight, out, error, abort, fit);
                case Icon::Payload::Jpeg2000:
                    return load_vips_buffer(file, entry.data, out, error, abort) ? Direct::Done : Direct::Failed;
                case Icon::Payload::Packed:
                    if (!Icon::unpack(entry, out)) {
                        fail(error, file, "icns decode failed");

                        return Direct::Failed;
                    }

                    return Direct::Done;
                case Icon::Payload::Dib: {
                    Bmp::Image image;

                    if (!Bmp::Image::open_icon(entry.data, &image)) {
                        fail(error, file, "ico entry unreadable");

                        return Direct::Failed;
                    }

                    const int factor = fit == Decode::Fit::Force ? shrink_factor(image.width(), image.height(), boxWidth, boxHeight) : 1;

                    if (!image.decode(factor, out, abort)) {
                        fail(error, file, aborted(abort) ? "aborted" : "ico decode failed");

                        return Direct::Failed;
                    }

                    return Direct::Done;
                }
            }

            return Direct::Skip;
        }
    }

    void Decode::Abort::request() {
        const std::scoped_lock hold(_guard);

        _requested.store(true, std::memory_order_relaxed);

        if (_image != nullptr) {
            vips_image_set_kill(_image, TRUE);
        }
    }

    void Decode::Abort::reset() {
        const std::scoped_lock hold(_guard);

        _requested.store(false, std::memory_order_relaxed);
    }

    void Decode::Abort::arm(_VipsImage *image) {
        const std::scoped_lock hold(_guard);

        _image = image;

        if (_requested.load(std::memory_order_relaxed)) {
            vips_image_set_kill(_image, TRUE);
        }
    }

    void Decode::Abort::disarm() {
        const std::scoped_lock hold(_guard);

        _image = nullptr;
    }

    std::span<const std::string_view> Decode::suffixes() {
        return SUFFIXES;
    }

    Decode::Format Decode::sniff(const std::span<const std::uint8_t> head) {
        if (starts_with(head, "\xFF\xD8\xFF")) {
            return Format::Jpeg;
        }

        if (starts_with(head, "\x89PNG\r\n\x1A\n")) {
            return Format::Png;
        }

        if (starts_with(head, "RIFF") && starts_with(head, "WEBP", 8)) {
            return Format::WebP;
        }

        if (starts_with(head, "\xFF\x0A") || starts_with(head, std::string_view("\0\0\0\x0CJXL \r\n\x87\n", 12))) {
            return Format::Jxl;
        }

        if (starts_with(head, "GIF8")) {
            return Format::Gif;
        }

        if (starts_with(head, "BM")) {
            return Format::Bmp;
        }

        if (starts_with(head, std::string_view("\0\0\1\0", 4)) && head.size() >= 6 && (head[4] != 0 || head[5] != 0)) {
            return Format::Ico;
        }

        if (starts_with(head, "icns")) {
            return Format::Icns;
        }

        if (starts_with(head, "II*\0") || starts_with(head, "MM\0*")) {
            return Format::Tiff;
        }

        if (starts_with(head, "ftyp", 4)) {
            return Format::Heif;
        }

        if (starts_with(head, "%PDF")) {
            return Format::Pdf;
        }

        if (starts_with(head, "<?xml") || starts_with(head, "<svg")) {
            return Format::Svg;
        }

        return Format::Other;
    }

    bool Decode::scales_cheaply(const Format format) {
        switch (format) {
            case Format::Jpeg:
            case Format::Svg:
            case Format::Pdf:
                return true;
            case Format::Png:
            case Format::WebP:
            case Format::Jxl:
            case Format::Gif:
            case Format::Bmp:
            case Format::Ico:
            case Format::Icns:
            case Format::Tiff:
            case Format::Heif:
            case Format::Other:
                break;
        }

        return false;
    }

    bool Decode::scalable(const Format format) {
        return format == Format::Svg || format == Format::Pdf;
    }

    bool Decode::recognised(const std::filesystem::path &file) {
        Mapped mapped;

        if (Mapped::open(file, &mapped) && sniff(mapped.data()) != Format::Other) {
            return true;
        }

        ensure_vips();

        const bool known = vips_foreign_find_load(file.string().c_str()) != nullptr;

        vips_error_clear();

        return known;
    }

    bool Decode::probe(const std::filesystem::path &file, Info *info, std::string *error) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped, error)) {
            return false;
        }

        *info = {};
        info->kind = sniff(mapped.data());
        info->format = format_name(info->kind);

        bool known = false;

        switch (info->kind) {
            case Format::Jpeg:
                known = probe_jpeg(mapped.data(), info);
                break;
            case Format::Png:
                known = probe_png(mapped.data(), info);
                break;
            case Format::WebP:
                known = probe_webp(mapped.data(), info);
                break;
            case Format::Jxl:
                known = probe_jxl(mapped.data(), info);
                break;
            case Format::Bmp:
                known = probe_bmp(mapped.data(), info);
                break;
            case Format::Ico:
            case Format::Icns:
                known = probe_icon(info->kind, mapped.data(), info);
                break;
            default:
                break;
        }

        if (known) {
            return true;
        }

        if (info->kind == Format::Heif) {
            const Heif::Colour colour = Heif::colour(mapped.data());

            info->hdr = colour.tone.hdr() || colour.gainMap;
        } else {
            info->hdr = container_tone(info->kind, mapped.data()).hdr();
        }

        mapped = {};

        return probe_vips(file, info, error);
    }

    bool Decode::load(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Abort *abort, const Fit fit, const Tone::Display &display) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped, error)) {
            return false;
        }

        const Format kind = sniff(mapped.data());
        Direct direct = Direct::Skip;

        switch (kind) {
            case Format::Jpeg:
                direct = load_jpeg(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit, display);
                break;
            case Format::Png:
                direct = load_png(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit, display);
                break;
            case Format::WebP:
                direct = fit == Fit::Cheap ? load_webp(file, mapped.data(), out, error, abort) : Direct::Skip;
                break;
            case Format::Jxl:
                direct = fit == Fit::Cheap ? load_jxl(file, mapped.data(), out, error, abort, display) : Direct::Skip;
                break;
            case Format::Bmp:
                direct = load_bmp(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            case Format::Ico:
            case Format::Icns:
                direct = load_icon(file, kind, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            default:
                break;
        }

        if (direct != Direct::Skip) {
            return direct == Direct::Done;
        }

        const Tone::Source tone = container_tone(kind, mapped.data());

        mapped = {};

        if (aborted(abort)) {
            fail(error, file, "aborted");

            return false;
        }

        Via via = Via::Whole;

        if (scalable(kind)) {
            via = Via::Scaled;
        } else if (scales_cheaply(kind)) {
            via = Via::Thumbnail;
        } else if (fit == Fit::Force) {
            via = Via::Shrink;
        }

        return load_vips(file, boxWidth, boxHeight, out, error, abort, via, tone, display);
    }

    bool Decode::render(const std::filesystem::path &file, const double scale, const int x, const int y, const int width, const int height, Bitmap *out, std::string *error, Abort *abort) {
        ensure_vips();

        try {
            const VImage image = VImage::new_from_file(file.string().c_str(), VImage::option()->set("scale", scale));
            const int left = std::clamp(x, 0, image.width() - 1);
            const int top = std::clamp(y, 0, image.height() - 1);
            const int partWidth = std::clamp(width, 1, image.width() - left);
            const int partHeight = std::clamp(height, 1, image.height() - top);

            if (Mapped mapped; Mapped::open(file, &mapped) && sniff(mapped.data()) == Format::Svg) {
                if (render_svg(mapped.data(), scale, {image.width(), image.height()}, {left, top, partWidth, partHeight}, out, abort)) {
                    return true;
                }

                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return false;
                }
            }

            const VImage part = image.crop(left, top, partWidth, partHeight);

            if (!write_rgba(prepare(part, {}), out, abort)) {
                fail(error, file, vips_error());

                return false;
            }

            return true;
        } catch (const vips::VError &) {
            if (abort != nullptr) {
                abort->disarm();
            }

            fail(error, file, vips_error());

            return false;
        }
    }

    bool Decode::load_frames(const std::filesystem::path &file, const std::size_t maxBytes, std::vector<Frame> *out, std::string *error, Abort *abort) {
        ensure_vips();

        try {
            // Sequential, so the frames stream through one by one instead of the whole strip decoding up front.
            VImage strip = VImage::new_from_file(file.string().c_str(), VImage::option()->set("n", -1)->set("access", VIPS_ACCESS_SEQUENTIAL));
            const int height = vips_image_get_page_height(strip.get_image());
            const int frames = strip.height() / height;
            const std::vector<int> delays = strip.get_typeof("delay") != 0 ? strip.get_array_int("delay") : std::vector<int>{};
            const int factor = frame_shrink(strip.width(), height, frames, maxBytes);
            const Tone::Source source = container_tone(file);
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

                if (!write_rgba(prepare(page, source), &next.bitmap, abort)) {
                    fail(error, file, vips_error());

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

            fail(error, file, vips_error());

            return false;
        }
    }

    bool Decode::stream(const std::filesystem::path &file, const int rows, const Begin &begin, const Take &take, std::string *error, Abort *abort, const Tone::Display &display) {
        if (Mapped mapped; Mapped::open(file, &mapped) && sniff(mapped.data()) == Format::Jxl) {
            const Direct direct = jxl_stream(mapped.data(), rows, begin, take, abort, display);

            if (direct != Direct::Skip) {
                if (direct == Direct::Failed) {
                    fail(error, file, aborted(abort) ? "aborted" : "jxl decode failed");
                }

                return direct == Direct::Done;
            }
        }

        ensure_vips();

        try {
            // Sequential, so each band decodes as it is asked for and nothing above it stays.
            VImage image = VImage::new_from_file(file.string().c_str(), VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL));
            const bool alpha = image.has_alpha();
            const Prepared prepared = prepare(image, container_tone(file), display);
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

                if (!write_rows(prepared, y, count, band.data(), abort)) {
                    fail(error, file, vips_error());

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

            fail(error, file, vips_error());

            return false;
        }
    }

    std::uint64_t Decode::stream_bytes(const std::filesystem::path &file, const int rows) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped) || sniff(mapped.data()) != Format::Jxl) {
            return 0;
        }

        JxlBasicInfo basic;
        Tone::Source tone;

        std::memset(&basic, 0, sizeof basic);

        if (!jxl_header(mapped.data(), &basic, &tone)) {
            return 0;
        }

        const double pixels = static_cast<double>(basic.xsize) * static_cast<double>(basic.ysize);
        const double band = static_cast<double>(basic.xsize) * static_cast<double>(rows) * Bitmap::CHANNELS;

        return static_cast<std::uint64_t>((pixels * (basic.uses_original_profile != 0 ? JXL_LOSSLESS_BYTES : JXL_LOSSY_BYTES)) + (band * (JxlBands::AHEAD + 1)));
    }

    bool Decode::load_png_memory(const std::span<const std::uint8_t> data, Bitmap *out, std::string *error) {
        constexpr int WHOLE = std::numeric_limits<int>::max();

        return load_png("memory", data, WHOLE, WHOLE, out, error, nullptr, Fit::Cheap) == Direct::Done;
    }

    void Decode::shutdown() {
        if (vips_state().started) {
            vips_shutdown();
        }
    }
}
