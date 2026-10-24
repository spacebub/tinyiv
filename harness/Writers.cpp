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
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
// jpeglib.h names FILE without including it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <jpeglib.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <libheif/heif.h>
#include <tiffio.h>
#include <webp/decode.h>
#include <zlib.h>

#include "Writers.h"

namespace harness::Writers {
    namespace {
        void put_u16(Bytes &out, const std::uint32_t value, const bool big) {
            if (big) {
                put_be(out, value, 2);
            } else {
                put_le(out, value, 2);
            }
        }

        void put_u32(Bytes &out, const std::uint32_t value, const bool big) {
            if (big) {
                put_be(out, value, 4);
            } else {
                put_le(out, value, 4);
            }
        }

        void set_le32(Bytes &out, const std::size_t at, const std::uint32_t value) {
            for (std::size_t i = 0; i < 4; ++i) {
                out.at(at + i) = static_cast<std::uint8_t>(value >> (8U * i));
            }
        }

        // Fractions of the binary gain map metadata keep this much of a stop.
        constexpr std::uint32_t GAIN_DENOMINATOR = 1U << 16U;

        void put_fraction(Bytes &out, const float value) {
            put_be(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value * GAIN_DENOMINATOR))),
                   4);
            put_be(out, GAIN_DENOMINATOR, 4);
        }

        int png_channels(const std::uint8_t colour) {
            switch (colour) {
                case 0:
                case 3:
                    return 1;
                case 4:
                    return 2;
                case 6:
                    return 4;
                default:
                    return 3;
            }
        }

        void png_chunk(Bytes &out, const std::string_view type, const std::span<const std::uint8_t> body) {
            put_be(out, static_cast<std::uint32_t>(body.size()), 4);

            const std::size_t start = out.size();

            put(out, type);
            put(out, body);
            put_be(out, static_cast<std::uint32_t>(crc32(0, &out.at(start), static_cast<uInt>(out.size() - start))), 4);
        }

        // Packs the samples from the rows into a PNG scanline, without the filter byte.
        void png_pack(const Png &spec, const std::span<const std::uint8_t> samples, const int count, const int step,
                      std::span<std::uint8_t> out) {
            const int channels = png_channels(spec.colour);

            std::ranges::fill(out, std::uint8_t{0});

            if (spec.depth == 16) {
                for (int i = 0; i < count; ++i) {
                    for (int c = 0; c < channels; ++c) {
                        const auto from = ((static_cast<std::size_t>(i * step) * channels) + c) * 2;
                        std::uint16_t value = 0;

                        std::memcpy(&value, &samples[from], 2);
                        out[((static_cast<std::size_t>(i) * channels) + c) * 2] =
                                static_cast<std::uint8_t>(value >> 8U);
                        out[(((static_cast<std::size_t>(i) * channels) + c) * 2) + 1] =
                                static_cast<std::uint8_t>(value & 0xFFU);
                    }
                }

                return;
            }

            if (spec.depth == 8) {
                for (int i = 0; i < count; ++i) {
                    for (int c = 0; c < channels; ++c) {
                        out[(static_cast<std::size_t>(i) * channels) + c] =
                                samples[(static_cast<std::size_t>(i * step) * channels) + c];
                    }
                }

                return;
            }

            const auto depth = static_cast<unsigned>(spec.depth);
            const unsigned perByte = 8U / depth;

            for (int i = 0; i < count; ++i) {
                const auto at = static_cast<unsigned>(i);
                const unsigned shift = 8U - (depth * ((at % perByte) + 1));

                out[at / perByte] = static_cast<std::uint8_t>(
                        out[at / perByte]
                        | ((static_cast<unsigned>(samples[static_cast<std::size_t>(i) * static_cast<std::size_t>(step)])
                            & ((1U << depth) - 1))
                           << shift));
            }
        }

        int paeth(const int a, const int b, const int c) {
            const int p = a + b - c;
            const int pa = std::abs(p - a);
            const int pb = std::abs(p - b);
            const int pc = std::abs(p - c);

            if (pa <= pb && pa <= pc) {
                return a;
            }

            return pb <= pc ? b : c;
        }

        // Picks the filter with the smallest sum of magnitudes, as libpng does.
        void png_filter(const std::span<const std::uint8_t> row, const std::span<const std::uint8_t> above,
                        const int bpp, Bytes &out) {
            const std::size_t length = row.size();
            std::array<Bytes, 5> tried;
            std::size_t best = 0;
            long bestSum = -1;

            for (std::size_t filter = 0; filter < tried.size(); ++filter) {
                Bytes &line = tried.at(filter);
                long sum = 0;

                line.resize(length + 1);
                line.at(0) = static_cast<std::uint8_t>(filter);

                for (std::size_t i = 0; i < length; ++i) {
                    const int a = std::cmp_greater_equal(i, bpp) ? row[i - static_cast<std::size_t>(bpp)] : 0;
                    const int b = above.empty() ? 0 : above[i];
                    const int c = std::cmp_greater_equal(i, bpp) && !above.empty()
                                          ? above[i - static_cast<std::size_t>(bpp)]
                                          : 0;
                    int predicted = 0;

                    switch (filter) {
                        case 1:
                            predicted = a;
                            break;
                        case 2:
                            predicted = b;
                            break;
                        case 3:
                            predicted = (a + b) / 2;
                            break;
                        case 4:
                            predicted = paeth(a, b, c);
                            break;
                        default:
                            break;
                    }

                    const auto value = static_cast<std::uint8_t>(row[i] - predicted);

                    line.at(i + 1) = value;
                    sum += std::abs(static_cast<int>(static_cast<std::int8_t>(value)));
                }

                if (bestSum < 0 || sum < bestSum) {
                    bestSum = sum;
                    best = filter;
                }
            }

            put(out, tried.at(best));
        }

        struct Deflated {
            Bytes bytes;
            uLong adler = 1;
            std::size_t raw = 0;
        };

        // Deflates into a raw stream that ends on a byte boundary. The last part ends with the final block.
        Deflated deflate_part(const std::span<const std::uint8_t> raw, const int level, const bool last) {
            z_stream stream{};

            if (deflateInit2(&stream, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                throw std::runtime_error("deflateInit2 failed");
            }

            Deflated held;

            held.bytes.resize(deflateBound(&stream, static_cast<uLong>(raw.size())) + 16);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib takes its input as non-const.
            stream.next_in = const_cast<Bytef *>(raw.data());
            stream.avail_in = static_cast<uInt>(raw.size());
            stream.next_out = held.bytes.data();
            stream.avail_out = static_cast<uInt>(held.bytes.size());

            const int status = deflate(&stream, last ? Z_FINISH : Z_SYNC_FLUSH);

            held.bytes.resize(stream.total_out);
            deflateEnd(&stream);

            if (status != (last ? Z_STREAM_END : Z_OK)) {
                throw std::runtime_error("deflate failed");
            }

            held.adler = adler32(1, raw.data(), static_cast<uInt>(raw.size()));
            held.raw = raw.size();

            return held;
        }

        // Cuts the zlib stream into IDAT chunks as it comes.
        class IdatSink {

        public:
            IdatSink(const std::function<void(std::span<const std::uint8_t>)> &out, const std::size_t size)
                : _out(&out), _size(size) {}

            void add(const std::span<const std::uint8_t> bytes) {
                put(_pending, bytes);

                while (_pending.size() >= _size) {
                    emit(_size);
                }
            }

            void finish() {
                if (!_pending.empty()) {
                    emit(_pending.size());
                }
            }

        private:
            void emit(const std::size_t count) {
                Bytes chunk;

                png_chunk(chunk, "IDAT", std::span(_pending).first(count));
                (*_out)(chunk);
                _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(count));
            }

            const std::function<void(std::span<const std::uint8_t>)> *_out;
            std::size_t _size;
            Bytes _pending;
        };

        std::size_t sample_bytes(const Png &spec) {
            return static_cast<std::size_t>(png_channels(spec.colour)) * (spec.depth == 16 ? 2 : 1);
        }

        std::size_t scanline_bytes(const Png &spec, const int width) {
            return ((static_cast<std::size_t>(width) * static_cast<std::size_t>(png_channels(spec.colour))
                     * static_cast<std::size_t>(spec.depth))
                    + 7)
                   / 8;
        }

        int png_bpp(const Png &spec) {
            return std::max(1, png_channels(spec.colour) * spec.depth / 8);
        }

        void png_header(const Png &spec, const std::function<void(std::span<const std::uint8_t>)> &out) {
            Bytes head = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            Bytes ihdr;

            put_be(ihdr, static_cast<std::uint32_t>(spec.width), 4);
            put_be(ihdr, static_cast<std::uint32_t>(spec.height), 4);
            ihdr.push_back(static_cast<std::uint8_t>(spec.depth));
            ihdr.push_back(spec.colour);
            ihdr.push_back(0);
            ihdr.push_back(0);
            ihdr.push_back(spec.interlaced ? 1 : 0);
            png_chunk(head, "IHDR", ihdr);

            if (!spec.cicp.empty()) {
                png_chunk(head, "cICP", spec.cicp);
            }

            if (!spec.exif.empty()) {
                png_chunk(head, "eXIf", spec.exif);
            }

            if (!spec.palette.empty()) {
                png_chunk(head, "PLTE", spec.palette);
            }

            if (!spec.transparency.empty()) {
                png_chunk(head, "tRNS", spec.transparency);
            }

            out(head);
        }

        // Filters each Adam7 pass separately and deflates all of them as one stream.
        void png_interlaced(const Png &spec, const Rows &rows, IdatSink &sink) {
            struct Pass {
                int x;
                int y;
                int dx;
                int dy;
            };

            constexpr std::array<Pass, 7> PASSES = {
                    {
                            {.x = 0, .y = 0, .dx = 8, .dy = 8},
                            {.x = 4, .y = 0, .dx = 8, .dy = 8},
                            {.x = 0, .y = 4, .dx = 4, .dy = 8},
                            {.x = 2, .y = 0, .dx = 4, .dy = 4},
                            {.x = 0, .y = 2, .dx = 2, .dy = 4},
                            {.x = 1, .y = 0, .dx = 2, .dy = 2},
                            {.x = 0, .y = 1, .dx = 1, .dy = 2},
                    },
            };

            const std::size_t pitch = sample_bytes(spec) * static_cast<std::size_t>(spec.width);
            Bytes whole(pitch * static_cast<std::size_t>(spec.height));
            Bytes filtered;

            rows(0, spec.height, whole);

            for (const Pass &pass : PASSES) {
                const int width = (spec.width - pass.x + pass.dx - 1) / pass.dx;
                const int height = (spec.height - pass.y + pass.dy - 1) / pass.dy;

                if (width <= 0 || height <= 0) {
                    continue;
                }

                Bytes above;
                Bytes line(scanline_bytes(spec, width));

                for (int y = pass.y; y < spec.height; y += pass.dy) {
                    const std::span<const std::uint8_t> row =
                            std::span(whole).subspan((pitch * static_cast<std::size_t>(y))
                                                     + (sample_bytes(spec) * static_cast<std::size_t>(pass.x)));

                    png_pack(spec, row, width, pass.dx, line);
                    png_filter(line, above, png_bpp(spec), filtered);
                    above = line;
                }
            }

            const Deflated stream = deflate_part(filtered, spec.level, true);
            Bytes framed = {0x78, 0x9C};

            put(framed, stream.bytes);
            put_be(framed, static_cast<std::uint32_t>(stream.adler), 4);
            sink.add(framed);
        }

        // Filters and deflates bands of rows side by side, then joins them into one zlib stream as pigz does.
        void png_banded(const Png &spec, const Rows &rows, IdatSink &sink) {
            constexpr std::size_t BAND_BYTES = std::size_t{4} << 20U;
            const std::size_t pitch = sample_bytes(spec) * static_cast<std::size_t>(spec.width);
            const std::size_t line = scanline_bytes(spec, spec.width);
            const int bandRows =
                    static_cast<int>(std::clamp<std::size_t>(BAND_BYTES / std::max<std::size_t>(line, 1), 16, 4096));
            const int bands = (spec.height + bandRows - 1) / bandRows;
            const int threads = std::max(1, std::min(16, static_cast<int>(std::thread::hardware_concurrency())));
            uLong adler = 1;
            const Bytes header = {0x78, 0x9C};

            sink.add(header);

            for (int first = 0; first < bands; first += threads) {
                const int wave = std::min(threads, bands - first);
                std::vector<Deflated> done(static_cast<std::size_t>(wave));

                {
                    std::vector<std::jthread> workers;

                    workers.reserve(wave);
                    for (int i = 0; i < wave; ++i) {
                        workers.emplace_back([&, i] {
                            const int top = (first + i) * bandRows;
                            const int count = std::min(bandRows, spec.height - top);
                            const int from = std::max(top - 1, 0);
                            Bytes samples(pitch * static_cast<std::size_t>(top + count - from));
                            Bytes above;
                            Bytes packed(line);
                            Bytes filtered;

                            rows(from, top + count - from, samples);
                            filtered.reserve((line + 1) * static_cast<std::size_t>(count));

                            for (int y = from; y < top + count; ++y) {
                                png_pack(spec, std::span(samples).subspan(pitch * static_cast<std::size_t>(y - from)),
                                         spec.width, 1, packed);

                                if (y >= top) {
                                    png_filter(packed, above, png_bpp(spec), filtered);
                                }

                                above = packed;
                            }

                            done.at(static_cast<std::size_t>(i)) =
                                    deflate_part(filtered, spec.level, first + i == bands - 1);
                        });
                    }
                }

                for (const Deflated &part : done) {
                    adler = adler32_combine(adler, part.adler, static_cast<z_off_t>(part.raw));
                    sink.add(part.bytes);
                }
            }

            Bytes trailer;

            put_be(trailer, static_cast<std::uint32_t>(adler), 4);
            sink.add(trailer);
        }

        void png_stream(const Png &spec, const Rows &rows,
                        const std::function<void(std::span<const std::uint8_t>)> &out) {
            png_header(spec, out);

            IdatSink sink(out, spec.idat);

            if (spec.interlaced) {
                png_interlaced(spec, rows, sink);
            } else {
                png_banded(spec, rows, sink);
            }

            sink.finish();

            Bytes end;

            png_chunk(end, "IEND", {});
            out(end);
        }

        struct JpegError {
            jpeg_error_mgr pub{};
            std::jmp_buf jump{};
            std::array<char, JMSG_LENGTH_MAX> message{};
        };

        [[noreturn]] void jpeg_fail(j_common_ptr info) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): pub is the first member, as libjpeg requires.
            auto *error = reinterpret_cast<JpegError *>(info->err);

            (*info->err->format_message)(info, error->message.data());
            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            std::longjmp(error->jump, 1);
        }

        void jpeg_quiet(j_common_ptr /*info*/) {
        }

        struct JpegWriter {
            jpeg_compress_struct info{};
            JpegError error;
            unsigned char *buffer = nullptr;
            unsigned long size = 0;

            JpegWriter() {
                info.err = jpeg_std_error(&error.pub);
                error.pub.error_exit = jpeg_fail;
                error.pub.output_message = jpeg_quiet;
                jpeg_create_compress(&info);
            }

            ~JpegWriter() {
                jpeg_destroy_compress(&info);
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): jpeg_mem_dest allocates with malloc.
                std::free(buffer);
            }

            JpegWriter(const JpegWriter &) = delete;
            JpegWriter(JpegWriter &&) = delete;
            JpegWriter &operator=(const JpegWriter &) = delete;
            JpegWriter &operator=(JpegWriter &&) = delete;
        };

        int jpeg_components(const JpegColour colour) {
            switch (colour) {
                case JpegColour::Grey:
                    return 1;
                case JpegColour::Cmyk:
                case JpegColour::Ycck:
                    return 4;
                default:
                    return 3;
            }
        }

        J_COLOR_SPACE colour_space(const int components) {
            switch (components) {
                case 1:
                    return JCS_GRAYSCALE;
                case 4:
                    return JCS_CMYK;
                default:
                    return JCS_RGB;
            }
        }

        // Returns false with the message in the writer on a libjpeg error.
        bool jpeg_run(JpegWriter &writer, const Jpeg &spec, const Rows &rows) {
            constexpr int BAND = 64;
            jpeg_compress_struct &info = writer.info;
            const int components = jpeg_components(spec.colour);

            // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
            if (setjmp(writer.error.jump) != 0) {
                return false;
            }

            jpeg_mem_dest(&info, &writer.buffer, &writer.size);
            info.image_width = static_cast<JDIMENSION>(spec.width);
            info.image_height = static_cast<JDIMENSION>(spec.height);
            info.input_components = components;
            info.in_color_space = colour_space(components);
            jpeg_set_defaults(&info);

            switch (spec.colour) {
                case JpegColour::Rgb:
                    jpeg_set_colorspace(&info, JCS_RGB);
                    break;
                case JpegColour::Cmyk:
                    jpeg_set_colorspace(&info, JCS_CMYK);
                    break;
                case JpegColour::Ycck:
                    jpeg_set_colorspace(&info, JCS_YCCK);
                    break;
                default:
                    break;
            }

            jpeg_set_quality(&info, spec.quality, TRUE);

            if (spec.lossless) {
                jpeg_enable_lossless(&info, 1, 0);
            }

            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): comp_info is an array of num_components.
            info.comp_info[0].h_samp_factor = spec.lossless ? 1 : spec.h;
            info.comp_info[0].v_samp_factor = spec.lossless ? 1 : spec.v;

            for (int c = 1; c < info.num_components; ++c) {
                info.comp_info[c].h_samp_factor = 1;
                info.comp_info[c].v_samp_factor = 1;
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

            info.write_JFIF_header = spec.jfif ? TRUE : FALSE;
            info.arith_code = spec.arithmetic ? TRUE : FALSE;
            info.optimize_coding = spec.arithmetic || spec.lossless ? FALSE : TRUE;

            if (spec.restartRows) {
                info.restart_in_rows = spec.restart;
            } else {
                info.restart_interval = static_cast<unsigned>(spec.restart);
            }

            if (spec.progressive) {
                jpeg_simple_progression(&info);
            }

            jpeg_start_compress(&info, TRUE);

            for (const auto &[marker, body] : spec.markers) {
                jpeg_write_marker(&info, marker, body.data(), static_cast<unsigned>(body.size()));
            }

            const std::size_t pitch = static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(components);
            Bytes band(pitch * BAND);
            std::vector<JSAMPROW> pointers(BAND);

            for (int top = 0; top < spec.height; top += BAND) {
                const int count = std::min(BAND, spec.height - top);

                rows(top, count, std::span(band).first(pitch * static_cast<std::size_t>(count)));

                for (int i = 0; i < count; ++i) {
                    pointers.at(static_cast<std::size_t>(i)) = &band.at(pitch * static_cast<std::size_t>(i));
                }

                for (int done = 0; done < count;) {
                    done += static_cast<int>(jpeg_write_scanlines(&info, &pointers.at(static_cast<std::size_t>(done)),
                                                                  static_cast<JDIMENSION>(count - done)));
                }
            }

            jpeg_finish_compress(&info);

            return true;
        }

        Bytes segment(const std::uint8_t marker, const std::span<const std::uint8_t> body) {
            Bytes held = {0xFF, marker};

            put_be(held, static_cast<std::uint32_t>(body.size() + 2), 2);
            put(held, body);

            return held;
        }

        // Returns the offset past SOI and any APP0 or APP1 segments at the start.
        std::size_t after_app_segments(const std::span<const std::uint8_t> data) {
            std::size_t at = 2;

            while (at + 4 <= data.size() && data[at] == 0xFF && (data[at + 1] == 0xE0 || data[at + 1] == 0xE1)) {
                at += 2 + ((static_cast<std::size_t>(data[at + 2]) << 8U) | data[at + 3]);
            }

            return at;
        }

        struct TiffCloser {
            void operator()(TIFF *handle) const { TIFFClose(handle); }
        };

        int tiff_compression(const TiffCompression compression) {
            switch (compression) {
                case TiffCompression::None:
                    return COMPRESSION_NONE;
                case TiffCompression::Lzw:
                    return COMPRESSION_LZW;
                case TiffCompression::Deflate:
                    return COMPRESSION_ADOBE_DEFLATE;
                case TiffCompression::PackBits:
                    return COMPRESSION_PACKBITS;
                case TiffCompression::Zstd:
                    return COMPRESSION_ZSTD;
                case TiffCompression::Jpeg:
                    return COMPRESSION_JPEG;
            }

            return COMPRESSION_NONE;
        }

        void tiff_check(const int status, const std::string_view what) {
            if (status != 1) {
                throw std::runtime_error(std::format("libtiff: {} failed", what));
            }
        }

        // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): TIFFSetField is variadic in C.
        void tiff_fields(TIFF *handle, const Tiff &spec) {
            const int compression = tiff_compression(spec.compression);

            if (TIFFIsCODECConfigured(static_cast<std::uint16_t>(compression)) == 0) {
                throw std::runtime_error("libtiff lacks the codec asked for");
            }

            tiff_check(TIFFSetField(handle, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(spec.width)), "width");
            tiff_check(TIFFSetField(handle, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(spec.height)), "height");
            tiff_check(TIFFSetField(handle, TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(spec.samples)),
                       "samples");
            tiff_check(TIFFSetField(handle, TIFFTAG_BITSPERSAMPLE, static_cast<std::uint16_t>(spec.bits)), "bits");
            tiff_check(
                    TIFFSetField(handle, TIFFTAG_SAMPLEFORMAT,
                                 static_cast<std::uint16_t>(spec.bits == 32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT)),
                    "sample format");
            tiff_check(
                    TIFFSetField(handle, TIFFTAG_PLANARCONFIG,
                                 static_cast<std::uint16_t>(spec.planar ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG)),
                    "planar");
            tiff_check(TIFFSetField(handle, TIFFTAG_COMPRESSION, static_cast<std::uint16_t>(compression)),
                       "compression");

            if (spec.compression == TiffCompression::Jpeg) {
                tiff_check(TIFFSetField(handle, TIFFTAG_PHOTOMETRIC, static_cast<std::uint16_t>(PHOTOMETRIC_YCBCR)),
                           "photometric");
                tiff_check(TIFFSetField(handle, TIFFTAG_JPEGQUALITY, 85), "quality");
                tiff_check(TIFFSetField(handle, TIFFTAG_YCBCRSUBSAMPLING, std::uint16_t{2}, std::uint16_t{2}),
                           "subsampling");
                tiff_check(TIFFSetField(handle, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB), "colour mode");
            } else {
                tiff_check(TIFFSetField(handle, TIFFTAG_PHOTOMETRIC, static_cast<std::uint16_t>(spec.photometric)),
                           "photometric");
            }

            if (spec.predictor) {
                tiff_check(TIFFSetField(handle, TIFFTAG_PREDICTOR,
                                        static_cast<std::uint16_t>(spec.bits == 32 ? PREDICTOR_FLOATINGPOINT
                                                                                   : PREDICTOR_HORIZONTAL)),
                           "predictor");
            }

            if (spec.extra >= 0) {
                const std::array<std::uint16_t, 1> kinds = {static_cast<std::uint16_t>(spec.extra)};

                tiff_check(TIFFSetField(handle, TIFFTAG_EXTRASAMPLES, std::uint16_t{1}, kinds.data()), "extra samples");
            }

            if (spec.orientation > 0) {
                tiff_check(TIFFSetField(handle, TIFFTAG_ORIENTATION, static_cast<std::uint16_t>(spec.orientation)),
                           "orientation");
            }

            if (!spec.colormap.empty()) {
                const std::size_t entries = spec.colormap.size() / 3;
                std::vector<std::uint16_t> map = spec.colormap;

                tiff_check(TIFFSetField(handle, TIFFTAG_COLORMAP, map.data(), &map.at(entries), &map.at(entries * 2)),
                           "colour map");
            }

            if (spec.tileWidth > 0) {
                tiff_check(TIFFSetField(handle, TIFFTAG_TILEWIDTH, static_cast<std::uint32_t>(spec.tileWidth)),
                           "tile width");
                tiff_check(TIFFSetField(handle, TIFFTAG_TILELENGTH, static_cast<std::uint32_t>(spec.tileHeight)),
                           "tile height");
            } else {
                tiff_check(TIFFSetField(handle, TIFFTAG_ROWSPERSTRIP, static_cast<std::uint32_t>(spec.stripRows)),
                           "rows per strip");
            }

            tiff_check(TIFFSetField(handle, TIFFTAG_SOFTWARE, "tiv_harness"), "software");
        }
        // NOLINTEND(cppcoreguidelines-pro-type-vararg)

        void tiff_tiles(TIFF *handle, const Tiff &spec, const Rows &rows) {
            const std::size_t pixel = static_cast<std::size_t>(spec.samples) * static_cast<std::size_t>(spec.bits / 8);
            const std::size_t pitch = pixel * static_cast<std::size_t>(spec.width);
            const auto tileWidth = static_cast<std::size_t>(spec.tileWidth);
            Bytes band(pitch * static_cast<std::size_t>(spec.tileHeight));
            Bytes tile(pixel * tileWidth * static_cast<std::size_t>(spec.tileHeight));

            for (int top = 0; top < spec.height; top += spec.tileHeight) {
                const int count = std::min(spec.tileHeight, spec.height - top);

                rows(top, count, std::span(band).first(pitch * static_cast<std::size_t>(count)));

                for (int left = 0; left < spec.width; left += spec.tileWidth) {
                    const auto across = static_cast<std::size_t>(std::min(spec.tileWidth, spec.width - left));

                    std::ranges::fill(tile, std::uint8_t{0});

                    for (int y = 0; y < count; ++y) {
                        std::memcpy(&tile.at(static_cast<std::size_t>(y) * tileWidth * pixel),
                                    &band.at((static_cast<std::size_t>(y) * pitch)
                                             + (static_cast<std::size_t>(left) * pixel)),
                                    across * pixel);
                    }

                    const std::uint32_t index = TIFFComputeTile(handle, static_cast<std::uint32_t>(left),
                                                                static_cast<std::uint32_t>(top), 0, 0);

                    if (TIFFWriteEncodedTile(handle, index, tile.data(), static_cast<tmsize_t>(tile.size())) < 0) {
                        throw std::runtime_error("libtiff: writing a tile failed");
                    }
                }
            }
        }

        void tiff_planes(TIFF *handle, const Tiff &spec, const Rows &rows) {
            const auto sample = static_cast<std::size_t>(spec.bits / 8);
            const std::size_t pitch =
                    sample * static_cast<std::size_t>(spec.samples) * static_cast<std::size_t>(spec.width);
            Bytes whole(pitch * static_cast<std::size_t>(spec.height));
            Bytes line(sample * static_cast<std::size_t>(spec.width));

            rows(0, spec.height, whole);

            for (int s = 0; s < spec.samples; ++s) {
                for (int y = 0; y < spec.height; ++y) {
                    for (int x = 0; x < spec.width; ++x) {
                        std::memcpy(&line.at(static_cast<std::size_t>(x) * sample),
                                    &whole.at((static_cast<std::size_t>(y) * pitch)
                                              + (((static_cast<std::size_t>(x) * static_cast<std::size_t>(spec.samples))
                                                  + static_cast<std::size_t>(s))
                                                 * sample)),
                                    sample);
                    }

                    if (TIFFWriteScanline(handle, line.data(), static_cast<std::uint32_t>(y),
                                          static_cast<std::uint16_t>(s))
                        < 0) {
                        throw std::runtime_error("libtiff: writing a row failed");
                    }
                }
            }
        }

        void tiff_strips(TIFF *handle, const Tiff &spec, const Rows &rows) {
            constexpr int BAND = 256;
            const std::size_t pitch = static_cast<std::size_t>(spec.samples) * static_cast<std::size_t>(spec.bits / 8)
                                      * static_cast<std::size_t>(spec.width);
            Bytes band(pitch * BAND);

            for (int top = 0; top < spec.height; top += BAND) {
                const int count = std::min(BAND, spec.height - top);

                rows(top, count, std::span(band).first(pitch * static_cast<std::size_t>(count)));

                for (int y = 0; y < count; ++y) {
                    if (TIFFWriteScanline(handle, &band.at(pitch * static_cast<std::size_t>(y)),
                                          static_cast<std::uint32_t>(top + y), 0)
                        < 0) {
                        throw std::runtime_error("libtiff: writing a row failed");
                    }
                }
            }
        }

        void heif_check(const heif_error error) {
            if (error.code != heif_error_Ok) {
                throw std::runtime_error(std::string("libheif: ") + error.message);
            }
        }

        struct HeifContextFree {
            void operator()(heif_context *context) const { heif_context_free(context); }
        };

        struct HeifEncoderFree {
            void operator()(heif_encoder *encoder) const { heif_encoder_release(encoder); }
        };

        struct HeifImageFree {
            void operator()(heif_image *image) const { heif_image_release(image); }
        };

        struct HeifHandleFree {
            void operator()(heif_image_handle *handle) const { heif_image_handle_release(handle); }
        };

        struct HeifOptionsFree {
            void operator()(heif_encoding_options *options) const { heif_encoding_options_free(options); }
        };

        struct HeifNclxFree {
            void operator()(heif_color_profile_nclx *nclx) const { heif_nclx_color_profile_free(nclx); }
        };

        using HeifImage = std::unique_ptr<heif_image, HeifImageFree>;

        // Returns part of the rows as an image of the given size. Edge pixels repeat where the part ends.
        HeifImage heif_image_of(const Heif &spec, const std::span<const std::uint8_t> band, const int left,
                                const int count, const int width, const int height,
                                const heif_color_profile_nclx *nclx) {
            const int channels = spec.alpha ? 4 : 3;
            const std::size_t sample = spec.bits > 8 ? 2 : 1;
            const std::size_t pixel = sample * static_cast<std::size_t>(channels);
            const std::size_t pitch = pixel * static_cast<std::size_t>(spec.width);
            heif_chroma chroma = spec.alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;

            if (spec.bits > 8) {
                chroma = spec.alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE;
            }

            heif_image *raw = nullptr;

            heif_check(heif_image_create(width, height, heif_colorspace_RGB, chroma, &raw));

            HeifImage image(raw);

            heif_check(heif_image_add_plane(image.get(), heif_channel_interleaved, width, height, spec.bits));

            std::size_t stride = 0;
            std::uint8_t *plane = heif_image_get_plane2(image.get(), heif_channel_interleaved, &stride);

            for (int y = 0; y < height; ++y) {
                const int from = std::min(y, count - 1);

                for (int x = 0; x < width; ++x) {
                    const int column = std::min(left + x, spec.width - 1);

                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): libheif hands out the plane as a pointer.
                    std::memcpy(plane + (static_cast<std::size_t>(y) * stride) + (static_cast<std::size_t>(x) * pixel),
                                &band[(static_cast<std::size_t>(from) * pitch)
                                      + (static_cast<std::size_t>(column) * pixel)],
                                pixel);
                }
            }

            if (nclx != nullptr) {
                heif_check(heif_image_set_nclx_color_profile(image.get(), nclx));
            }

            return image;
        }

        void jxl_check(const JxlEncoderStatus status, const std::string_view what) {
            if (status != JXL_ENC_SUCCESS) {
                throw std::runtime_error(std::format("libjxl: {} failed", what));
            }
        }

        struct JxlEncoderFree {
            void operator()(JxlEncoder *encoder) const { JxlEncoderDestroy(encoder); }
        };

        struct JxlRunnerFree {
            void operator()(void *runner) const { JxlThreadParallelRunnerDestroy(runner); }
        };

        JxlColorEncoding jxl_encoding(const Jxl &spec) {
            JxlColorEncoding encoding;
            const JXL_BOOL grey = spec.channels < 3 ? JXL_TRUE : JXL_FALSE;

            if (spec.colour == JxlColour::LinearSrgb) {
                JxlColorEncodingSetToLinearSRGB(&encoding, grey);
            } else {
                JxlColorEncodingSetToSRGB(&encoding, grey);
            }

            if (spec.colour == JxlColour::Pq || spec.colour == JxlColour::Hlg) {
                encoding.primaries = JXL_PRIMARIES_2100;
                encoding.white_point = JXL_WHITE_POINT_D65;
                encoding.transfer_function =
                        spec.colour == JxlColour::Pq ? JXL_TRANSFER_FUNCTION_PQ : JXL_TRANSFER_FUNCTION_HLG;
                encoding.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
            }

            return encoding;
        }

        void box(Bytes &out, const std::string_view type, const std::span<const std::uint8_t> body) {
            put_be(out, static_cast<std::uint32_t>(body.size() + 8), 4);
            put(out, type);
            put(out, body);
        }

        class Lzw {

        public:
            Lzw(Bytes &out, const int minimum) : _out(&out), _minimum(minimum) { reset(); }

            void encode(const std::span<const std::uint8_t> indices) {
                emit(clear());

                if (indices.empty()) {
                    emit(clear() + 1);
                    flush();

                    return;
                }

                int current = indices.front();

                for (const std::uint8_t pixel : indices.subspan(1)) {
                    const auto key = (static_cast<std::uint32_t>(current) << 8U) | pixel;

                    if (const auto found = _codes.find(key); found != _codes.end()) {
                        current = found->second;

                        continue;
                    }

                    emit(current);
                    current = pixel;

                    // A full table is cleared and never extended, as giflib does.
                    if (_next >= 4095) {
                        emit(clear());
                        reset();
                    } else {
                        _codes.emplace(key, _next++);
                    }
                }

                emit(current);
                emit(clear() + 1);
                flush();
            }

        private:
            [[nodiscard]] int clear() const { return static_cast<int>(1U << static_cast<unsigned>(_minimum)); }

            void reset() {
                _codes.clear();
                _width = _minimum + 1;
                _next = clear() + 2;
            }

            // Codes go out least significant bit first. The width grows once the next code no longer fits.
            void emit(const int code) {
                _bits |= static_cast<std::uint32_t>(code) << static_cast<unsigned>(_count);
                _count += _width;

                while (_count >= 8) {
                    _block.push_back(static_cast<std::uint8_t>(_bits & 0xFFU));
                    _bits >>= 8U;
                    _count -= 8;
                    spill();
                }

                if (std::cmp_greater_equal(_next, 1U << static_cast<unsigned>(_width)) && _width < 12) {
                    ++_width;
                }
            }

            void spill() {
                if (_block.size() == 255) {
                    _out->push_back(255);
                    put(*_out, _block);
                    _block.clear();
                }
            }

            void flush() {
                if (_count > 0) {
                    _block.push_back(static_cast<std::uint8_t>(_bits & 0xFFU));
                    _bits = 0;
                    _count = 0;
                }

                if (!_block.empty()) {
                    _out->push_back(static_cast<std::uint8_t>(_block.size()));
                    put(*_out, _block);
                    _block.clear();
                }

                _out->push_back(0);
            }

            Bytes *_out;
            int _minimum;
            int _width = 0;
            int _next = 0;
            std::unordered_map<std::uint32_t, int> _codes;
            std::uint32_t _bits = 0;
            int _count = 0;
            Bytes _block;
        };

        // Returns the table size field of a GIF palette for the number of entries.
        int gif_size_bits(const std::size_t entries) {
            int bits = 1;

            while ((std::size_t{1} << static_cast<unsigned>(bits)) < entries) {
                ++bits;
            }

            return bits;
        }

        void gif_palette(Bytes &out, const Bytes &palette) {
            const std::size_t entries = std::size_t{1} << static_cast<unsigned>(gif_size_bits(palette.size() / 3));

            put(out, palette);
            out.resize(out.size() + (entries * 3) - palette.size(), 0);
        }

        // Rows in the order of the four interlace passes.
        Bytes gif_interlace(const GifFrame &frame) {
            Bytes held;
            const auto width = static_cast<std::size_t>(frame.width);

            for (const auto &[start, step] : {std::pair{0, 8}, std::pair{4, 8}, std::pair{2, 4}, std::pair{1, 2}}) {
                for (int y = start; y < frame.height; y += step) {
                    put(held, std::span(frame.indices).subspan(static_cast<std::size_t>(y) * width, width));
                }
            }

            return held;
        }

        std::uint32_t scale_to_mask(const std::uint8_t value, const std::uint32_t mask) {
            if (mask == 0) {
                return 0;
            }

            const auto shift = static_cast<std::uint32_t>(std::countr_zero(mask));
            const std::uint32_t top = mask >> shift;

            return (((static_cast<std::uint32_t>(value) * top) + 127) / 255) << shift;
        }

        // One stored row of a BMP that is not run length coded.
        void bmp_row(const Bmp &spec, const std::span<const std::uint8_t> pixels, const int y, Bytes &out) {
            const auto width = static_cast<std::size_t>(spec.width);
            const std::size_t start = out.size();
            const std::size_t stride = ((width * static_cast<std::size_t>(spec.depth)) + 31) / 32 * 4;

            out.resize(start + stride, 0);

            const std::span<std::uint8_t> row = std::span(out).subspan(start, stride);

            if (spec.depth <= 8) {
                const auto depth = static_cast<unsigned>(spec.depth);
                const unsigned perByte = 8U / depth;

                for (std::size_t x = 0; x < width; ++x) {
                    const unsigned shift = 8U - (depth * ((static_cast<unsigned>(x) % perByte) + 1));

                    row[x / perByte] = static_cast<std::uint8_t>(
                            row[x / perByte]
                            | ((pixels[(static_cast<std::size_t>(y) * width) + x] & ((1U << depth) - 1)) << shift));
                }

                return;
            }

            const std::array<std::uint32_t, 4> masks = spec.depth == 16 && spec.compression == 0
                                                               ? std::array<std::uint32_t, 4>{0x7C00, 0x03E0, 0x001F, 0}
                                                               : spec.masks;

            for (std::size_t x = 0; x < width; ++x) {
                const std::span<const std::uint8_t> rgba =
                        pixels.subspan(((static_cast<std::size_t>(y) * width) + x) * 4, 4);

                if (spec.depth == 24) {
                    row[x * 3] = rgba[2];
                    row[(x * 3) + 1] = rgba[1];
                    row[(x * 3) + 2] = rgba[0];
                } else if (spec.depth == 32 && spec.compression == 0) {
                    row[x * 4] = rgba[2];
                    row[(x * 4) + 1] = rgba[1];
                    row[(x * 4) + 2] = rgba[0];
                    row[(x * 4) + 3] = rgba[3];
                } else {
                    std::uint32_t value = 0;

                    for (std::size_t c = 0; c < 4; ++c) {
                        value |= scale_to_mask(rgba[c], masks.at(c));
                    }

                    const std::size_t bytes = static_cast<std::size_t>(spec.depth) / 8;

                    for (std::size_t b = 0; b < bytes; ++b) {
                        row[(x * bytes) + b] = static_cast<std::uint8_t>(value >> (8U * b));
                    }
                }
            }
        }

        std::uint8_t pair(const std::uint8_t high, const std::uint8_t low) {
            return static_cast<std::uint8_t>((static_cast<unsigned>(high) << 4U) | low);
        }

        std::size_t run_at(const std::span<const std::uint8_t> indices, const std::size_t at) {
            std::size_t length = 1;

            while (at + length < indices.size() && length < 255 && indices[at + length] == indices[at]) {
                ++length;
            }

            return length;
        }

        // Padded to a whole number of 16 bit words, as BMP asks.
        void put_absolute(const std::span<const std::uint8_t> run, const bool nibbles, Bytes &out) {
            std::size_t bytes = run.size();

            out.push_back(0);
            out.push_back(static_cast<std::uint8_t>(run.size()));

            if (nibbles) {
                bytes = 0;

                for (std::size_t i = 0; i < run.size(); i += 2) {
                    out.push_back(pair(run[i], i + 1 < run.size() ? run[i + 1] : 0));
                    ++bytes;
                }
            } else {
                put(out, run);
            }

            if (bytes % 2 != 0) {
                out.push_back(0);
            }
        }

        // Too short for an absolute run. An RLE4 run alternates its two nibbles, an RLE8 run cannot.
        void put_short(const std::span<const std::uint8_t> run, const bool nibbles, Bytes &out) {
            if (nibbles) {
                out.push_back(static_cast<std::uint8_t>(run.size()));
                out.push_back(pair(run.front(), run.back()));

                return;
            }

            for (const std::uint8_t index : run) {
                out.push_back(1);
                out.push_back(index);
            }
        }

        // Codes runs of three or more as encoded runs. The rest go out as absolute runs where they are long enough.
        void rle_row(const std::span<const std::uint8_t> indices, const bool nibbles, Bytes &out) {
            const std::size_t width = indices.size();

            for (std::size_t x = 0; x < width;) {
                const std::size_t run = run_at(indices, x);

                if (run >= 3) {
                    out.push_back(static_cast<std::uint8_t>(run));
                    out.push_back(nibbles ? pair(indices[x], indices[x]) : indices[x]);
                    x += run;

                    continue;
                }

                std::size_t literal = 0;

                while (x + literal < width && literal < 254 && run_at(indices, x + literal) < 3) {
                    ++literal;
                }

                const std::span<const std::uint8_t> part = indices.subspan(x, literal);

                if (literal < 3) {
                    put_short(part, nibbles, out);
                } else {
                    put_absolute(part, nibbles, out);
                }

                x += literal;
            }
        }

        Bytes bmp_rle(const Bmp &spec, const std::span<const std::uint8_t> pixels) {
            Bytes held;
            const auto width = static_cast<std::size_t>(spec.width);
            const bool nibbles = spec.compression == 2;

            for (int line = 0; line < spec.height; ++line) {
                if (spec.deltaRows > 0 && line == spec.deltaRow) {
                    held.insert(held.end(), {0, 2, 0, static_cast<std::uint8_t>(spec.deltaRows)});
                    line += spec.deltaRows;
                }

                if (line >= spec.height) {
                    break;
                }

                rle_row(pixels.subspan(static_cast<std::size_t>(spec.height - 1 - line) * width, width), nibbles, held);
                held.insert(held.end(), {0, 0});
            }

            held.insert(held.end(), {0, 1});

            return held;
        }

        void bmp_header(const Bmp &spec, const std::size_t imageBytes, const bool icon, Bytes &out) {
            const auto size = static_cast<std::uint32_t>(spec.header);
            const int height = icon ? spec.height * 2 : spec.height;

            put_le(out, size, 4);

            if (spec.header == BmpHeader::Core) {
                put_le(out, static_cast<std::uint32_t>(spec.width), 2);
                put_le(out, static_cast<std::uint32_t>(height), 2);
                put_le(out, 1, 2);
                put_le(out, static_cast<std::uint32_t>(spec.depth), 2);

                return;
            }

            put_le(out, static_cast<std::uint32_t>(spec.width), 4);
            put_le(out, static_cast<std::uint32_t>(spec.topDown ? -height : height), 4);
            put_le(out, 1, 2);
            put_le(out, static_cast<std::uint32_t>(spec.depth), 2);

            if (spec.header == BmpHeader::Os2Short) {
                return;
            }

            put_le(out, spec.compression, 4);
            put_le(out, static_cast<std::uint32_t>(imageBytes), 4);
            put_le(out, 2835, 4);
            put_le(out, 2835, 4);
            put_le(out, spec.used, 4);
            put_le(out, 0, 4);

            if (size >= static_cast<std::uint32_t>(BmpHeader::V2) && spec.header != BmpHeader::Os2) {
                for (std::size_t c = 0; c < (size >= static_cast<std::uint32_t>(BmpHeader::V3) ? 4U : 3U); ++c) {
                    put_le(out, spec.masks.at(c), 4);
                }
            }

            if (spec.header == BmpHeader::V4 || spec.header == BmpHeader::V5) {
                put(out, "BGRs");
                out.resize(out.size() + 36 + 12, 0);
            }

            if (spec.header == BmpHeader::V5) {
                put_le(out, 4, 4);
                out.resize(out.size() + 12, 0);
            }

            // The OS/2 2.x fields past the Windows ones stay zero.
            out.resize(std::max<std::size_t>(out.size(), size), 0);
        }

        // An icon keeps a 1 bit transparency mask after the pixels. It comes from the alpha, or below 16 bits from index 0.
        void put_icon_mask(const Bmp &spec, const std::span<const std::uint8_t> pixels, Bytes &image) {
            const std::size_t stride = (static_cast<std::size_t>(spec.width) + 31) / 32 * 4;

            for (int i = 0; i < spec.height; ++i) {
                const int y = spec.height - 1 - i;
                Bytes row(stride, 0);

                for (int x = 0; x < spec.width; ++x) {
                    const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(spec.width))
                                    + static_cast<std::size_t>(x);
                    const bool clear = spec.depth > 8 ? pixels[(at * 4) + 3] < 128 : pixels[at] == 0;

                    if (clear) {
                        row.at(static_cast<std::size_t>(x) / 8) = static_cast<std::uint8_t>(
                                row.at(static_cast<std::size_t>(x) / 8) | (0x80U >> (static_cast<unsigned>(x) % 8)));
                    }
                }

                put(image, row);
            }
        }

        // Writes the bit field masks that follow an INFO header, then the palette.
        void put_colour_table(const Bmp &spec, Bytes &info) {
            const bool masksAfter = spec.header == BmpHeader::Info && (spec.compression == 3 || spec.compression == 6);

            if (masksAfter) {
                for (std::size_t c = 0; c < (spec.compression == 6 ? 4U : 3U); ++c) {
                    put_le(info, spec.masks.at(c), 4);
                }
            }

            if (!spec.palette.empty()) {
                const std::size_t entries = spec.palette.size() / 3;

                for (std::size_t i = 0; i < entries; ++i) {
                    info.push_back(spec.palette.at((i * 3) + 2));
                    info.push_back(spec.palette.at((i * 3) + 1));
                    info.push_back(spec.palette.at(i * 3));

                    if (spec.header != BmpHeader::Core) {
                        info.push_back(0);
                    }
                }
            }
        }
    }

    void save(const std::filesystem::path &file, const std::span<const std::uint8_t> bytes) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streams write chars.
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

        if (!out) {
            throw std::runtime_error("could not write " + file.string());
        }
    }

    void put(Bytes &out, const std::string_view text) {
        out.insert(out.end(), text.begin(), text.end());
    }

    void put(Bytes &out, const std::span<const std::uint8_t> bytes) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    void put_le(Bytes &out, const std::uint32_t value, const int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i))));
        }
    }

    void put_be(Bytes &out, const std::uint32_t value, const int bytes) {
        for (int i = bytes - 1; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i))));
        }
    }

    Bytes exif(const int orientation, const bool bigEndian) {
        struct Tag {
            std::uint16_t tag;
            std::uint16_t type;
            Bytes value;
        };

        constexpr std::uint16_t ASCII = 2;
        constexpr std::uint16_t SHORT = 3;
        constexpr std::uint16_t RATIONAL = 5;

        const auto text = [](const std::string_view value) {
            Bytes held;

            put(held, value);
            held.push_back(0);

            return held;
        };
        const auto short_value = [&](const int value) {
            Bytes held;

            put_u16(held, static_cast<std::uint32_t>(value), bigEndian);
            put_u16(held, 0, bigEndian);

            return held;
        };
        Bytes resolution;

        put_u32(resolution, 72, bigEndian);
        put_u32(resolution, 1, bigEndian);

        std::vector<Tag> tags = {
                {.tag = 0x010E, .type = ASCII, .value = text("tiv_harness corpus")},
                {.tag = 0x010F, .type = ASCII, .value = text("tinyiv")},
                {.tag = 0x011A, .type = RATIONAL, .value = resolution},
                {.tag = 0x011B, .type = RATIONAL, .value = resolution},
                {.tag = 0x0128, .type = SHORT, .value = short_value(2)},
                {.tag = 0x0131, .type = ASCII, .value = text("tiv_harness")},
        };

        if (orientation > 0) {
            tags.insert(tags.begin() + 2, {.tag = 0x0112, .type = SHORT, .value = short_value(orientation)});
        }

        Bytes block;

        put(block, bigEndian ? "MM" : "II");
        put_u16(block, 0x2A, bigEndian);
        put_u32(block, 8, bigEndian);
        put_u16(block, static_cast<std::uint32_t>(tags.size()), bigEndian);

        std::size_t outside = 8 + 2 + (tags.size() * 12) + 4;
        Bytes data;

        for (const Tag &tag : tags) {
            put_u16(block, tag.tag, bigEndian);
            put_u16(block, tag.type, bigEndian);
            put_u32(block, tag.type == ASCII ? static_cast<std::uint32_t>(tag.value.size()) : 1, bigEndian);

            if (tag.value.size() <= 4) {
                Bytes inline_value = tag.value;

                inline_value.resize(4, 0);
                put(block, inline_value);
            } else {
                put_u32(block, static_cast<std::uint32_t>(outside), bigEndian);
                put(data, tag.value);
                outside += tag.value.size();

                if (outside % 2 != 0) {
                    data.push_back(0);
                    ++outside;
                }
            }
        }

        put_u32(block, 0, bigEndian);
        put(block, data);

        return block;
    }

    Bytes png(const Png &spec, const Rows &rows) {
        Bytes held;

        png_stream(spec, rows, [&held](const std::span<const std::uint8_t> bytes) { put(held, bytes); });

        return held;
    }

    void write_png(const std::filesystem::path &file, const Png &spec, const Rows &rows) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);

        png_stream(spec, rows, [&out](const std::span<const std::uint8_t> bytes) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streams write chars.
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        });

        if (!out) {
            throw std::runtime_error("could not write " + file.string());
        }
    }

    Bytes jpeg(const Jpeg &spec, const Rows &rows) {
        JpegWriter writer;

        if (!jpeg_run(writer, spec, rows)) {
            throw std::runtime_error(std::string("libjpeg: ") + writer.error.message.data());
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): jpeg_mem_dest hands back a pointer and a size.
        return {writer.buffer, writer.buffer + writer.size};
    }

    Bytes gain_iso(const Gain &gain, const bool versioned) {
        constexpr std::uint8_t BASE_COLOURS = 0x40;
        Bytes held;

        if (versioned) {
            held.push_back(0);
        }

        put_be(held, 0, 2);
        put_be(held, 0, 2);
        held.push_back(gain.baseColours ? BASE_COLOURS : 0);
        put_fraction(held, gain.baseHeadroom);
        put_fraction(held, gain.alternateHeadroom);
        put_fraction(held, gain.min);
        put_fraction(held, gain.max);
        put_fraction(held, gain.gamma);
        put_fraction(held, gain.baseOffset);
        put_fraction(held, gain.alternateOffset);

        return held;
    }

    std::string gain_xmp(const Gain &gain) {
        return std::format(R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                           R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                           R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/")"
                           R"( hdrgm:Version="1.0" hdrgm:GainMapMin="{}" hdrgm:GainMapMax="{}" hdrgm:Gamma="{}")"
                           R"( hdrgm:OffsetSDR="{}" hdrgm:OffsetHDR="{}" hdrgm:HDRCapacityMin="{}")"
                           R"( hdrgm:HDRCapacityMax="{}" hdrgm:BaseRenditionIsHDR="False"/>)"
                           R"(</rdf:RDF></x:xmpmeta>)",
                           gain.min, gain.max, gain.gamma, gain.baseOffset, gain.alternateOffset, gain.baseHeadroom,
                           gain.alternateHeadroom);
    }

    Bytes jpeg_with_gain_map(const std::span<const std::uint8_t> primary, const std::span<const std::uint8_t> map) {
        constexpr std::uint16_t MP_VERSION = 0xB000;
        constexpr std::uint16_t MP_IMAGES = 0xB001;
        constexpr std::uint16_t MP_ENTRY = 0xB002;
        constexpr std::uint16_t UNDEFINED = 7;
        constexpr std::uint16_t LONG = 4;
        constexpr std::uint32_t PRIMARY = 0x030000;
        constexpr std::size_t TABLE = 8 + 2 + (3 * 12) + 4;
        constexpr std::size_t BODY = 4 + TABLE + 32;

        const std::size_t insert = after_app_segments(primary);
        // Offsets count from the TIFF header, past the marker, the length and the signature.
        const std::size_t origin = insert + 4 + 4;
        const std::size_t primarySize = primary.size() + 4 + BODY;
        Bytes body;

        put(body, std::string_view("MPF\0", 4));
        put(body, "II");
        put_le(body, 0x2A, 2);
        put_le(body, 8, 4);
        put_le(body, 3, 2);
        put_le(body, MP_VERSION, 2);
        put_le(body, UNDEFINED, 2);
        put_le(body, 4, 4);
        put(body, "0100");
        put_le(body, MP_IMAGES, 2);
        put_le(body, LONG, 2);
        put_le(body, 1, 4);
        put_le(body, 2, 4);
        put_le(body, MP_ENTRY, 2);
        put_le(body, UNDEFINED, 2);
        put_le(body, 32, 4);
        put_le(body, static_cast<std::uint32_t>(TABLE), 4);
        put_le(body, 0, 4);
        put_le(body, PRIMARY, 4);
        put_le(body, static_cast<std::uint32_t>(primarySize), 4);
        put_le(body, 0, 4);
        put_le(body, 0, 4);
        put_le(body, 0, 4);
        put_le(body, static_cast<std::uint32_t>(map.size()), 4);
        put_le(body, static_cast<std::uint32_t>(primarySize - origin), 4);
        put_le(body, 0, 4);

        Bytes held(primary.begin(), primary.begin() + static_cast<std::ptrdiff_t>(insert));

        put(held, segment(0xE2, body));
        put(held, primary.subspan(insert));
        put(held, map);

        return held;
    }

    void write_tiff(const std::filesystem::path &file, const Tiff &spec, const Rows &rows) {
        const std::unique_ptr<TIFF, TiffCloser> handle(TIFFOpen(file.c_str(), spec.bigEndian ? "wb" : "wl"));

        if (handle == nullptr) {
            throw std::runtime_error("libtiff could not open " + file.string());
        }

        tiff_fields(handle.get(), spec);

        if (spec.tileWidth > 0) {
            tiff_tiles(handle.get(), spec, rows);
        } else if (spec.planar) {
            tiff_planes(handle.get(), spec, rows);
        } else {
            tiff_strips(handle.get(), spec, rows);
        }

        if (TIFFWriteDirectory(handle.get()) != 1) {
            throw std::runtime_error("libtiff could not finish " + file.string());
        }
    }

    void write_heif(const std::filesystem::path &file, const Heif &spec, const Rows &rows) {
        const std::unique_ptr<heif_context, HeifContextFree> context(heif_context_alloc());
        heif_encoder *rawEncoder = nullptr;

        heif_check(heif_context_get_encoder_for_format(
                context.get(), spec.av1 ? heif_compression_AV1 : heif_compression_HEVC, &rawEncoder));

        const std::unique_ptr<heif_encoder, HeifEncoderFree> encoder(rawEncoder);

        heif_check(heif_encoder_set_lossy_quality(encoder.get(), spec.quality));
        // Speed settings differ by plugin. A plugin that lacks one is no reason to stop.
        std::ignore = heif_encoder_set_parameter_string(encoder.get(), "preset", "ultrafast");
        std::ignore = heif_encoder_set_parameter_integer(encoder.get(), "speed", 9);

        const std::unique_ptr<heif_encoding_options, HeifOptionsFree> options(heif_encoding_options_alloc());
        std::unique_ptr<heif_color_profile_nclx, HeifNclxFree> nclx;

        options->save_alpha_channel = spec.alpha ? 1 : 0;
        options->image_orientation = static_cast<heif_orientation>(spec.orientation);

        if (spec.primaries != 0) {
            nclx.reset(heif_nclx_color_profile_alloc());
            nclx->color_primaries = static_cast<heif_color_primaries>(spec.primaries);
            nclx->transfer_characteristics = static_cast<heif_transfer_characteristics>(spec.transfer);
            nclx->matrix_coefficients = static_cast<heif_matrix_coefficients>(spec.matrix);
            nclx->full_range_flag = 1;
            options->output_nclx_profile = nclx.get();
        }

        const std::size_t pixel = std::size_t{spec.bits > 8 ? 2U : 1U} * (spec.alpha ? 4U : 3U);
        const std::size_t pitch = pixel * static_cast<std::size_t>(spec.width);

        if (spec.columns * spec.rows <= 1) {
            Bytes whole(pitch * static_cast<std::size_t>(spec.height));

            rows(0, spec.height, whole);

            const HeifImage image = heif_image_of(spec, whole, 0, spec.height, spec.width, spec.height, nclx.get());
            heif_image_handle *handle = nullptr;

            heif_check(heif_context_encode_image(context.get(), image.get(), encoder.get(), options.get(), &handle));
            heif_image_handle_release(handle);
        } else {
            const int tileWidth = (spec.width + spec.columns - 1) / spec.columns;
            const int tileHeight = (spec.height + spec.rows - 1) / spec.rows;
            heif_image_handle *rawGrid = nullptr;

            heif_check(heif_context_add_grid_image(context.get(), static_cast<std::uint32_t>(spec.width),
                                                   static_cast<std::uint32_t>(spec.height),
                                                   static_cast<std::uint32_t>(spec.columns),
                                                   static_cast<std::uint32_t>(spec.rows), options.get(), &rawGrid));

            const std::unique_ptr<heif_image_handle, HeifHandleFree> grid(rawGrid);
            Bytes band(pitch * static_cast<std::size_t>(tileHeight));

            for (int row = 0; row < spec.rows; ++row) {
                const int top = row * tileHeight;
                const int count = std::min(tileHeight, spec.height - top);

                rows(top, count, std::span(band).first(pitch * static_cast<std::size_t>(count)));

                for (int column = 0; column < spec.columns; ++column) {
                    const HeifImage tile =
                            heif_image_of(spec, band, column * tileWidth, count, tileWidth, tileHeight, nclx.get());

                    heif_check(heif_context_add_image_tile(context.get(), grid.get(),
                                                           static_cast<std::uint32_t>(column),
                                                           static_cast<std::uint32_t>(row), tile.get(), encoder.get()));
                }
            }
        }

        heif_check(heif_context_write_to_file(context.get(), file.c_str()));
    }

    Bytes jxl(const Jxl &spec, const Rows &rows) {
        const std::unique_ptr<JxlEncoder, JxlEncoderFree> encoder(JxlEncoderCreate(nullptr));
        const std::unique_ptr<void, JxlRunnerFree> runner(JxlThreadParallelRunnerCreate(
                nullptr, std::min<std::size_t>(16, JxlThreadParallelRunnerDefaultNumWorkerThreads())));

        jxl_check(JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get()), "runner");
        jxl_check(JxlEncoderUseContainer(encoder.get(), spec.container ? JXL_TRUE : JXL_FALSE), "container");

        const bool alpha = spec.channels == 2 || spec.channels == 4;
        const bool animated = spec.durations.size() > 1;
        JxlBasicInfo info;

        JxlEncoderInitBasicInfo(&info);
        info.xsize = static_cast<std::uint32_t>(spec.width);
        info.ysize = static_cast<std::uint32_t>(spec.height);
        info.bits_per_sample = static_cast<std::uint32_t>(spec.bits);
        info.exponent_bits_per_sample = spec.bits == 32 ? 8 : 0;
        info.num_color_channels = spec.channels >= 3 ? 3 : 1;
        info.num_extra_channels = alpha ? 1 : 0;
        info.alpha_bits = alpha ? static_cast<std::uint32_t>(spec.bits) : 0;
        info.alpha_exponent_bits = alpha && spec.bits == 32 ? 8 : 0;
        info.uses_original_profile = spec.lossless ? JXL_TRUE : JXL_FALSE;
        info.orientation = static_cast<JxlOrientation>(spec.orientation);

        if (spec.colour == JxlColour::Pq) {
            info.intensity_target = 10000.0F;
        }

        if (animated) {
            info.have_animation = JXL_TRUE;
            info.animation.tps_numerator = 1000;
            info.animation.tps_denominator = 1;
            info.animation.num_loops = 0;
        }

        jxl_check(JxlEncoderSetBasicInfo(encoder.get(), &info), "basic info");

        const JxlColorEncoding encoding = jxl_encoding(spec);

        jxl_check(JxlEncoderSetColorEncoding(encoder.get(), &encoding), "colour encoding");

        JxlEncoderFrameSettings *settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);

        jxl_check(JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, spec.effort), "effort");

        if (spec.lossless) {
            jxl_check(JxlEncoderSetFrameLossless(settings, JXL_TRUE), "lossless");
        } else {
            jxl_check(JxlEncoderSetFrameDistance(settings, spec.distance), "distance");
        }

        JxlDataType type = JXL_TYPE_UINT8;

        if (spec.bits == 16) {
            type = JXL_TYPE_UINT16;
        } else if (spec.bits == 32) {
            type = JXL_TYPE_FLOAT;
        }

        const JxlPixelFormat format = {
                .num_channels = static_cast<std::uint32_t>(spec.channels),
                .data_type = type,
                .endianness = JXL_NATIVE_ENDIAN,
                .align = 0,
        };
        const std::size_t pitch = static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.channels)
                                  * static_cast<std::size_t>(spec.bits / 8);
        const std::size_t frames = std::max<std::size_t>(spec.durations.size(), 1);
        Bytes frame(pitch * static_cast<std::size_t>(spec.height));

        for (std::size_t f = 0; f < frames; ++f) {
            if (animated) {
                JxlFrameHeader header;

                JxlEncoderInitFrameHeader(&header);
                header.duration = static_cast<std::uint32_t>(spec.durations.at(f));
                jxl_check(JxlEncoderSetFrameHeader(settings, &header), "frame header");
            }

            rows(static_cast<int>(f) * spec.height, spec.height, frame);
            jxl_check(JxlEncoderAddImageFrame(settings, &format, frame.data(), frame.size()), "frame");
        }

        JxlEncoderCloseInput(encoder.get());

        Bytes held(std::size_t{1} << 20U);
        std::size_t used = 0;

        while (true) {
            std::uint8_t *next = &held.at(used);
            std::size_t left = held.size() - used;
            const JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &next, &left);

            used = held.size() - left;

            if (status == JXL_ENC_SUCCESS) {
                break;
            }

            if (status != JXL_ENC_NEED_MORE_OUTPUT) {
                throw std::runtime_error("libjxl: encoding failed");
            }

            held.resize(held.size() * 2);
        }

        held.resize(used);

        return held;
    }

    Bytes jxl_with_gain_map(const std::span<const std::uint8_t> codestream, const Gain &gain,
                            const std::span<const std::uint8_t> map) {
        const Bytes metadata = gain_iso(gain);
        Bytes held = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};
        Bytes type;
        Bytes bundle = {0};

        put(type, "jxl ");
        put_be(type, 0, 4);
        put(type, "jxl ");
        box(held, "ftyp", type);

        put_be(bundle, static_cast<std::uint32_t>(metadata.size()), 2);
        put(bundle, metadata);
        bundle.push_back(0);
        put_be(bundle, 0, 4);
        put(bundle, map);
        box(held, "jhgm", bundle);
        box(held, "jxlc", codestream);

        return held;
    }

    Bytes gif(const Gif &spec) {
        Bytes held;

        put(held, "GIF89a");
        put_le(held, static_cast<std::uint32_t>(spec.width), 2);
        put_le(held, static_cast<std::uint32_t>(spec.height), 2);
        held.push_back(static_cast<std::uint8_t>(0x80U | 0x70U
                                                 | static_cast<unsigned>(gif_size_bits(spec.palette.size() / 3) - 1)));
        held.push_back(0);
        held.push_back(0);
        gif_palette(held, spec.palette);

        if (spec.loop >= 0) {
            held.insert(held.end(), {0x21, 0xFF, 0x0B});
            put(held, "NETSCAPE2.0");
            held.insert(held.end(), {0x03, 0x01});
            put_le(held, static_cast<std::uint32_t>(spec.loop), 2);
            held.push_back(0);
        }

        for (const GifFrame &frame : spec.frames) {
            const bool keyed = frame.transparent >= 0;

            held.insert(held.end(), {0x21, 0xF9, 0x04});
            held.push_back(
                    static_cast<std::uint8_t>((static_cast<unsigned>(frame.disposal) << 2U) | (keyed ? 1U : 0U)));
            put_le(held, static_cast<std::uint32_t>(frame.delay), 2);
            held.push_back(static_cast<std::uint8_t>(keyed ? frame.transparent : 0));
            held.push_back(0);

            const Bytes &palette = frame.palette.empty() ? spec.palette : frame.palette;
            const int bits = gif_size_bits(palette.size() / 3);
            unsigned packed = frame.interlaced ? 0x40U : 0U;

            if (!frame.palette.empty()) {
                packed |= 0x80U | static_cast<unsigned>(bits - 1);
            }

            held.push_back(0x2C);
            put_le(held, static_cast<std::uint32_t>(frame.x), 2);
            put_le(held, static_cast<std::uint32_t>(frame.y), 2);
            put_le(held, static_cast<std::uint32_t>(frame.width), 2);
            put_le(held, static_cast<std::uint32_t>(frame.height), 2);
            held.push_back(static_cast<std::uint8_t>(packed));

            if (!frame.palette.empty()) {
                gif_palette(held, frame.palette);
            }

            const int minimum = std::max(2, bits);

            held.push_back(static_cast<std::uint8_t>(minimum));

            Lzw lzw(held, minimum);

            lzw.encode(frame.interlaced ? gif_interlace(frame) : frame.indices);
        }

        held.push_back(0x3B);

        return held;
    }

    Bytes bmp(const Bmp &spec, const std::span<const std::uint8_t> pixels, const bool icon) {
        const bool packed = spec.compression == 1 || spec.compression == 2;
        Bytes image;

        if (packed) {
            image = bmp_rle(spec, pixels);
        } else {
            for (int i = 0; i < spec.height; ++i) {
                bmp_row(spec, pixels, spec.topDown ? i : spec.height - 1 - i, image);
            }
        }

        if (icon) {
            put_icon_mask(spec, pixels, image);
        }

        Bytes info;

        bmp_header(spec, packed ? image.size() : 0, icon, info);

        put_colour_table(spec, info);

        Bytes held;

        if (!icon) {
            const std::size_t offset = 14 + info.size();

            put(held, "BM");
            put_le(held, static_cast<std::uint32_t>(offset + image.size()), 4);
            put_le(held, 0, 4);
            put_le(held, static_cast<std::uint32_t>(offset), 4);
        }

        put(held, info);
        put(held, image);
        held.resize(held.size() - std::min(spec.truncate, held.size()));

        return held;
    }

    Bytes ico(const std::vector<IcoEntry> &entries) {
        Bytes held;
        std::size_t offset = 6 + (16 * entries.size());

        put_le(held, 0, 2);
        put_le(held, 1, 2);
        put_le(held, static_cast<std::uint32_t>(entries.size()), 2);

        for (const IcoEntry &entry : entries) {
            held.push_back(static_cast<std::uint8_t>(entry.width >= 256 ? 0 : entry.width));
            held.push_back(static_cast<std::uint8_t>(entry.height >= 256 ? 0 : entry.height));
            held.push_back(static_cast<std::uint8_t>(entry.depth < 8 ? 1U << static_cast<unsigned>(entry.depth) : 0U));
            held.push_back(0);
            put_le(held, 1, 2);
            put_le(held, static_cast<std::uint32_t>(entry.depth), 2);
            put_le(held, static_cast<std::uint32_t>(entry.data.size()), 4);
            put_le(held, static_cast<std::uint32_t>(offset), 4);
            offset += entry.data.size();
        }

        for (const IcoEntry &entry : entries) {
            put(held, entry.data);
        }

        return held;
    }

    Bytes icns_packed(const std::span<const std::uint8_t> rgba) {
        constexpr std::size_t LONGEST_RUN = 130;
        constexpr std::size_t LONGEST_LITERAL = 128;
        const std::size_t count = rgba.size() / 4;
        Bytes held;

        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto at = [&](const std::size_t i) { return rgba[(i * 4) + channel]; };

            for (std::size_t i = 0; i < count;) {
                std::size_t run = 1;

                while (i + run < count && run < LONGEST_RUN && at(i + run) == at(i)) {
                    ++run;
                }

                if (run >= 3) {
                    held.push_back(static_cast<std::uint8_t>(0x80 + run - 3));
                    held.push_back(at(i));
                    i += run;

                    continue;
                }

                std::size_t literal = 0;

                while (i + literal < count && literal < LONGEST_LITERAL
                       && (i + literal + 2 >= count || at(i + literal) != at(i + literal + 1)
                           || at(i + literal) != at(i + literal + 2))) {
                    ++literal;
                }

                held.push_back(static_cast<std::uint8_t>(literal - 1));

                for (std::size_t k = 0; k < literal; ++k) {
                    held.push_back(at(i + k));
                }

                i += literal;
            }
        }

        return held;
    }

    Bytes icns(const std::vector<std::pair<std::string, Bytes>> &chunks) {
        Bytes body;

        for (const auto &[tag, data] : chunks) {
            put(body, tag);
            put_be(body, static_cast<std::uint32_t>(data.size() + 8), 4);
            put(body, data);
        }

        Bytes held;

        put(held, "icns");
        put_be(held, static_cast<std::uint32_t>(body.size() + 8), 4);
        put(held, body);

        return held;
    }

    Bytes pdf(const std::vector<PdfPage> &pages) {
        std::vector<std::size_t> offsets;
        Bytes held;
        const auto object = [&](const std::string &dictionary, const std::span<const std::uint8_t> stream) {
            offsets.push_back(held.size());
            put(held, std::format("{} 0 obj\n{}\n", offsets.size(), dictionary));

            if (!stream.empty() || dictionary.contains("/Length")) {
                put(held, "stream\n");
                put(held, stream);
                put(held, "\nendstream\n");
            }

            put(held, "endobj\n");
        };

        put(held, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

        // Catalog, page tree and font come first, then each page with its content and photo.
        std::string kids;
        std::size_t next = 4;

        for (const PdfPage &page : pages) {
            kids += std::format("{} 0 R ", next);
            next += page.photo.empty() ? 2 : 3;
        }

        object("<< /Type /Catalog /Pages 2 0 R >>", {});
        object(std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, pages.size()), {});
        object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>", {});

        for (const PdfPage &page : pages) {
            const std::size_t self = offsets.size() + 1;
            const std::string photo =
                    page.photo.empty() ? std::string() : std::format(" /XObject << /Photo {} 0 R >>", self + 2);
            Bytes content;

            put(content, page.content);

            if (page.deflate) {
                uLongf size = compressBound(static_cast<uLong>(content.size()));
                Bytes packed(size);

                compress(packed.data(), &size, content.data(), static_cast<uLong>(content.size()));
                packed.resize(size);
                content = std::move(packed);
            }

            object(std::format("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {} {}] /Resources << /Font << /F1 3 0 R "
                               ">>{} >> /Contents {} 0 R >>",
                               page.width, page.height, photo, self + 1),
                   {});
            object(std::format("<< /Length {}{} >>", content.size(), page.deflate ? " /Filter /FlateDecode" : ""),
                   content);

            if (!page.photo.empty()) {
                object(std::format("<< /Type /XObject /Subtype /Image /Width {} /Height {} /ColorSpace /DeviceRGB "
                                   "/BitsPerComponent 8 /Filter /DCTDecode /Length {} >>",
                                   page.photoWidth, page.photoHeight, page.photo.size()),
                       page.photo);
            }
        }

        const std::size_t xref = held.size();

        put(held, std::format("xref\n0 {}\n0000000000 65535 f \n", offsets.size() + 1));

        for (const std::size_t offset : offsets) {
            put(held, std::format("{:010} 00000 n \n", offset));
        }

        put(held, std::format("trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n", offsets.size() + 1, xref));

        return held;
    }

    Bytes webp_with_exif(const std::span<const std::uint8_t> simple, const std::span<const std::uint8_t> exif) {
        constexpr std::uint8_t FLAG_EXIF = 0x08;
        constexpr std::uint8_t FLAG_ALPHA = 0x10;
        constexpr std::size_t RIFF_HEADER = 12;
        Bytes held(simple.begin(), simple.begin() + RIFF_HEADER);
        const bool extended = simple.size() > RIFF_HEADER + 8
                              && std::ranges::equal(simple.subspan(RIFF_HEADER, 4), std::string_view("VP8X"), {}, {},
                                                    [](const char c) { return static_cast<std::uint8_t>(c); });

        if (extended) {
            put(held, simple.subspan(RIFF_HEADER));
            held.at(RIFF_HEADER + 8) = static_cast<std::uint8_t>(held.at(RIFF_HEADER + 8) | FLAG_EXIF);
        } else {
            WebPBitstreamFeatures features;

            if (WebPGetFeatures(simple.data(), simple.size(), &features) != VP8_STATUS_OK) {
                throw std::runtime_error("not a WebP");
            }

            put(held, "VP8X");
            put_le(held, 10, 4);
            held.push_back(static_cast<std::uint8_t>(FLAG_EXIF | (features.has_alpha != 0 ? FLAG_ALPHA : 0U)));
            held.insert(held.end(), {0, 0, 0});
            put_le(held, static_cast<std::uint32_t>(features.width - 1), 3);
            put_le(held, static_cast<std::uint32_t>(features.height - 1), 3);
            put(held, simple.subspan(RIFF_HEADER));
        }

        put(held, "EXIF");
        put_le(held, static_cast<std::uint32_t>(exif.size()), 4);
        put(held, exif);

        if (exif.size() % 2 != 0) {
            held.push_back(0);
        }

        set_le32(held, 4, static_cast<std::uint32_t>(held.size() - 8));

        return held;
    }
}
