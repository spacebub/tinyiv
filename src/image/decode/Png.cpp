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
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <png.h>

#include "image/Bitmap.h"
#include "image/Exif.h"
#include "image/Orient.h"
#include "image/Shrink.h"
#include "image/Tone.h"
#include "image/decode/Png.h"
#include "image/decode/Support.h"

namespace tiv::Decode {
    namespace {
        // What a band of wide PNG rows waiting for the tone mapper may take.
        constexpr std::size_t PNG_BAND_BYTES = std::size_t{1} << 20;

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
    }

    Tone::Source Png::tone(const std::span<const std::uint8_t> data) {
        const std::span<const std::uint8_t> cicp = png_chunk(data, "cICP");

        return cicp.size() >= 2 ? Tone::from_cicp(cicp[0], cicp[1]) : Tone::Source{};
    }

    bool Png::probe(const std::span<const std::uint8_t> data, Decode::Info *info) {
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
        info->hdr = tone(data).hdr();

        return width > 0 && height > 0;
    }

    Direct Png::load(const std::filesystem::path &file, const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, const Decode::Abort *abort, const Decode::Fit fit,
                     const Tone::Display &display) {
        PngHandle handle;
        PngReader reader{data, 0};
        const Tone::Source source = tone(data);

        if (!png_open(handle, reader, source.hdr())) {
            fail(error, file, "png header unreadable");

            return Direct::Failed;
        }

        const int orientation = png_orientation(handle);
        const auto width = static_cast<int>(png_get_image_width(handle.png, handle.info));
        const auto height = static_cast<int>(png_get_image_height(handle.png, handle.info));
        const int factor = fit == Decode::Fit::Force ? shrink_factor(width, height, Orient::swaps(orientation) ? boxHeight : boxWidth, Orient::swaps(orientation) ? boxWidth : boxHeight) : 1;

        if ((factor > 1 || source.hdr()) && png_get_interlace_type(handle.png, handle.info) != PNG_INTERLACE_NONE) {
            return Direct::Skip;
        }

        const std::optional<Tone::Mapper> mapper = source.hdr() ? std::optional(Tone::Mapper(source, display)) : std::nullopt;
        const Tone::Mapper *mapping = mapper ? &*mapper : nullptr;
        const Bitmap::Encoding encoding = source.hdr() && display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;
        Bitmap held = Bitmap::allocate((width + factor - 1) / factor, (height + factor - 1) / factor, encoding);
        const bool read = factor > 1 ? png_read_shrunk(handle, held, factor, mapping, abort) : png_read(handle, held, mapping, abort);

        if (!read) {
            fail(error, file, aborted(abort) ? "aborted" : "png decode failed");

            return Direct::Failed;
        }

        *out = std::move(held);

        return Direct::Done;
    }
}
