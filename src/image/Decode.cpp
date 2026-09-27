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
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
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
#include "image/Icon.h"
#include "image/Mapped.h"
#include "image/Orient.h"
#include "image/Shrink.h"
#include "image/Svg.h"

namespace tiv {
    namespace {
        using vips::VImage;

        constexpr int MAX_VIPS_THREADS = 8;

        // The incremental WebP decoder is fed this much between abort checks.
        constexpr std::size_t WEBP_CHUNK = std::size_t{8} * 1024 * 1024;

        // The direct decoders check for an abort every this many rows.
        constexpr int ABORT_ROWS = 64;

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

        bool probe_vips(const std::filesystem::path &file, Decode::Info *info, std::string *error) {
            ensure_vips();

            try {
                const VImage image = VImage::new_from_file(file.string().c_str());
                const bool swapped = vips_image_get_orientation_swap(image.get_image()) != 0;

                info->width = swapped ? image.height() : image.width();
                info->height = swapped ? image.width() : image.height();
                info->orientation = vips_image_get_orientation(image.get_image());
                info->frames = animates(info->kind) ? std::max(vips_image_get_n_pages(image.get_image()), 1) : 1;

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

        // Renders the image into a new bitmap, which must be RGBA8 already.
        bool write_rgba(const VImage &image, Bitmap *out, Decode::Abort *abort) {
            Bitmap held = Bitmap::allocate(image.width(), image.height());
            const VImage target = VImage::new_from_memory(held.data(), held.bytes(), held.width(), held.height(), Bitmap::CHANNELS, VIPS_FORMAT_UCHAR);

            if (abort != nullptr) {
                abort->arm(image.get_image());
            }

            const bool written = vips_image_write(image.get_image(), target.get_image()) == 0;

            if (abort != nullptr) {
                abort->disarm();
            }

            if (written) {
                *out = std::move(held);
            }

            return written;
        }

        bool load_vips(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Decode::Abort *abort, const Via via) {
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

                if (!write_rgba(to_rgba(image), out, abort)) {
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
                if (!write_rgba(to_rgba(VImage::new_from_buffer(data.data(), data.size(), "")), out, abort)) {
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

                return write_rgba(to_rgba(drawn.crop(piece.x - left, piece.y - top, piece.width, piece.height)), out, nullptr);
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

            return true;
        }

        Direct load_jpeg(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit) {
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

        // Reads the header and sets the transforms that make every PNG come out RGBA8.
        bool png_open(PngHandle &handle, PngReader &reader) {
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

            if (depth == 16) {
                png_set_scale_16(handle.png);
            }

            if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) {
                png_set_gray_to_rgb(handle.png);
            }

            if ((colour & PNG_COLOR_MASK_ALPHA) == 0 && png_get_valid(handle.png, handle.info, PNG_INFO_tRNS) == 0) {
                png_set_filler(handle.png, 0xFF, PNG_FILLER_AFTER);
            }

            return true;
        }

        bool png_read(PngHandle &handle, Bitmap &target, const Decode::Abort *abort) {
            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp): libpng reports errors by longjmp only.
            if (setjmp(png_jmpbuf(handle.png)) != 0) {
                return false;
            }

            const int passes = png_set_interlace_handling(handle.png);

            png_read_update_info(handle.png, handle.info);

            if (png_get_rowbytes(handle.png, handle.info) != target.pitch() || png_get_channels(handle.png, handle.info) != Bitmap::CHANNELS) {
                return false;
            }

            for (int pass = 0; pass < passes; ++pass) {
                for (int y = 0; y < target.height(); ++y) {
                    if (y % ABORT_ROWS == 0 && aborted(abort)) {
                        return false;
                    }

                    png_read_row(handle.png, target.row(y).data(), nullptr);
                }
            }

            png_read_end(handle.png, nullptr);

            return true;
        }

        // Rows stream through a box filter into the target, so an image of any size costs its
        // shrunk size plus two rows. Only for images that are not interlaced.
        bool png_read_shrunk(PngHandle &handle, Bitmap &target, const int factor, const Decode::Abort *abort) {
            const auto width = static_cast<int>(png_get_image_width(handle.png, handle.info));
            const auto height = static_cast<int>(png_get_image_height(handle.png, handle.info));
            std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * Bitmap::CHANNELS);
            BoxShrink shrink(width, height, factor, &target);

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp): libpng reports errors by longjmp only.
            if (setjmp(png_jmpbuf(handle.png)) != 0) {
                return false;
            }

            png_read_update_info(handle.png, handle.info);

            if (png_get_rowbytes(handle.png, handle.info) != row.size() || png_get_channels(handle.png, handle.info) != Bitmap::CHANNELS) {
                return false;
            }

            for (int y = 0; y < height; ++y) {
                if (y % ABORT_ROWS == 0 && aborted(abort)) {
                    return false;
                }

                png_read_row(handle.png, row.data(), nullptr);
                shrink.push(row);
            }

            png_read_end(handle.png, nullptr);

            return true;
        }

        bool probe_png(const std::span<const std::uint8_t> data, Decode::Info *info) {
            // Width and height sit at fixed offsets in IHDR, the first chunk: https://www.w3.org/TR/png-3/#11IHDR
            constexpr std::size_t IHDR = 16;

            if (data.size() < IHDR + 8) {
                return false;
            }

            const auto read = [&](const std::size_t at) {
                return static_cast<int>((static_cast<std::uint32_t>(data[at]) << 24) | (static_cast<std::uint32_t>(data[at + 1]) << 16)
                                        | (static_cast<std::uint32_t>(data[at + 2]) << 8) | data[at + 3]);
            };

            const int width = read(IHDR);
            const int height = read(IHDR + 4);

            // eXIf comes before the image data: https://www.w3.org/TR/png-3/#eXIf
            for (std::size_t at = 8; at + 12 <= data.size() && !starts_with(data, "IDAT", at + 4);) {
                const auto length = static_cast<std::size_t>(static_cast<std::uint32_t>(read(at)));

                if (at + 12 + length > data.size()) {
                    break;
                }

                if (starts_with(data, "eXIf", at + 4)) {
                    info->orientation = Exif::orientation(data.subspan(at + 8, length));

                    break;
                }

                at += 12 + length;
            }

            info->width = Orient::swaps(info->orientation) ? height : width;
            info->height = Orient::swaps(info->orientation) ? width : height;

            return width > 0 && height > 0;
        }

        Direct load_png(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit) {
            PngHandle handle;
            PngReader reader{data, 0};

            if (!png_open(handle, reader)) {
                fail(error, file, "png header unreadable");

                return Direct::Failed;
            }

            const int orientation = png_orientation(handle);
            const auto width = static_cast<int>(png_get_image_width(handle.png, handle.info));
            const auto height = static_cast<int>(png_get_image_height(handle.png, handle.info));
            const int factor = fit == Decode::Fit::Force ? shrink_factor(width, height, Orient::swaps(orientation) ? boxHeight : boxWidth, Orient::swaps(orientation) ? boxWidth : boxHeight) : 1;

            if (factor > 1 && png_get_interlace_type(handle.png, handle.info) != PNG_INTERLACE_NONE) {
                return Direct::Skip;
            }

            Bitmap held = Bitmap::allocate((width + factor - 1) / factor, (height + factor - 1) / factor);
            const bool read = factor > 1 ? png_read_shrunk(handle, held, factor, abort) : png_read(handle, held, abort);

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

        bool probe_jxl(const std::span<const std::uint8_t> data, Decode::Info *info) {
            const JxlHandle handle;

            if (handle.decoder == nullptr || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return false;
            }

            JxlDecoderCloseInput(handle.decoder);

            if (JxlDecoderProcessInput(handle.decoder) != JXL_DEC_BASIC_INFO) {
                return false;
            }

            JxlBasicInfo basic;

            if (JxlDecoderGetBasicInfo(handle.decoder, &basic) != JXL_DEC_SUCCESS) {
                return false;
            }

            const Size size = jxl_size(basic);

            info->width = size.width;
            info->height = size.height;
            info->orientation = static_cast<int>(basic.orientation);

            return true;
        }

        // libjxl with its own thread pool, which libvips does not use. Animation stays with libvips.
        Direct load_jxl(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, const Decode::Abort *abort) {
            const JxlHandle handle;

            if (handle.decoder == nullptr || handle.runner == nullptr
                || JxlDecoderSetParallelRunner(handle.decoder, JxlThreadParallelRunner, handle.runner) != JXL_DEC_SUCCESS
                || JxlDecoderSetKeepOrientation(handle.decoder, JXL_TRUE) != JXL_DEC_SUCCESS
                || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return Direct::Skip;
            }

            JxlDecoderCloseInput(handle.decoder);

            const JxlPixelFormat format{Bitmap::CHANNELS, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
            Bitmap held;

            for (;;) {
                if (aborted(abort)) {
                    fail(error, file, "aborted");

                    return Direct::Failed;
                }

                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    JxlBasicInfo info;

                    if (JxlDecoderGetBasicInfo(handle.decoder, &info) != JXL_DEC_SUCCESS || info.have_animation != 0) {
                        return Direct::Skip;
                    }

                    held = Bitmap::allocate(static_cast<int>(info.xsize), static_cast<int>(info.ysize));
                } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    std::size_t needed = 0;

                    if (held.empty() || JxlDecoderImageOutBufferSize(handle.decoder, &format, &needed) != JXL_DEC_SUCCESS || needed != held.bytes()
                        || JxlDecoderSetImageOutBuffer(handle.decoder, &format, held.data(), held.bytes()) != JXL_DEC_SUCCESS) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
                    break;
                } else {
                    fail(error, file, "jxl decode failed");

                    return Direct::Failed;
                }
            }

            if (held.empty()) {
                return Direct::Skip;
            }

            *out = std::move(held);

            return Direct::Done;
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

        mapped = {};

        return probe_vips(file, info, error);
    }

    bool Decode::load(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Abort *abort, const Fit fit) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped, error)) {
            return false;
        }

        const Format kind = sniff(mapped.data());
        Direct direct = Direct::Skip;

        switch (kind) {
            case Format::Jpeg:
                direct = load_jpeg(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            case Format::Png:
                direct = load_png(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            case Format::WebP:
                direct = fit == Fit::Cheap ? load_webp(file, mapped.data(), out, error, abort) : Direct::Skip;
                break;
            case Format::Jxl:
                direct = fit == Fit::Cheap ? load_jxl(file, mapped.data(), out, error, abort) : Direct::Skip;
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

        return load_vips(file, boxWidth, boxHeight, out, error, abort, via);
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

            if (!write_rgba(to_rgba(part), out, abort)) {
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
            std::vector<Frame> held;

            strip = to_rgba(strip);
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

                if (!write_rgba(page, &next.bitmap, abort)) {
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
