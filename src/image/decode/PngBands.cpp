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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>


#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <zlib.h>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/FileReader.h"
#include "image/Simd.h"
#include "image/decode/Decode.h"
#include "image/decode/Png.h"
#include "image/decode/PngBands.h"
#include "image/decode/Support.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,portability-simd-intrinsics): rows and chunks are walked by offset, the filter with intrinsics.
namespace tiv::Decode {
    namespace {
        constexpr std::array<std::uint8_t, 8> SIGNATURE = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        constexpr std::size_t CHUNK_HEAD = 8;
        constexpr std::size_t CRC = 4;
        constexpr std::size_t TYPE = 4;
        // Chunks before the image data are read whole, which a sane file keeps far below this.
        constexpr std::uint64_t MAX_HEAD = std::uint64_t{16} << 20U;
        constexpr std::size_t READ_BYTES = std::size_t{1} << 20U;
        constexpr std::size_t INFLATE_BYTES = std::size_t{256} * 1024;
        // What inflateInit2 is told for raw deflate, the zlib wrapper being skipped by hand.
        constexpr int RAW_DEFLATE = -15;
        constexpr std::size_t ZLIB_WRAPPER = 2;
        // Each piece of a band pays for its own inflate state and first row, so a thread takes at least
        // this many rows.
        constexpr int MIN_ROWS = 16;
        constexpr std::uint32_t OPAQUE = 0xFF000000U;
        constexpr std::uint8_t CLEAR = 0;
        constexpr std::uint8_t SOLID = 0xFF;

        // PNG's colour types: https://www.w3.org/TR/png-3/#6Colour-values
        enum class Colour : std::uint8_t {
            Grey = 0,
            Rgb = 2,
            Palette = 3,
            GreyAlpha = 4,
            Rgba = 6,
        };

        [[nodiscard]] Colour colour_of(const std::uint8_t code) {
            return static_cast<Colour>(code);
        }

        [[nodiscard]] int samples_of(const std::uint8_t code) {
            switch (colour_of(code)) {
                case Colour::Grey:
                case Colour::Palette:
                    return 1;
                case Colour::GreyAlpha:
                    return 2;
                case Colour::Rgb:
                    return 3;
                case Colour::Rgba:
                    return 4;
            }

            return 0;
        }

        [[nodiscard]] std::uint32_t big32(const std::uint8_t *at) {
            return (static_cast<std::uint32_t>(at[0]) << 24U) | (static_cast<std::uint32_t>(at[1]) << 16U)
                   | (static_cast<std::uint32_t>(at[2]) << 8U) | at[3];
        }

        [[nodiscard]] bool named(const std::span<const std::uint8_t> type, const char *name) {
            return std::memcmp(type.data(), name, TYPE) == 0;
        }

        // The compressed stream across its IDAT chunks, from a place in one, a large read at a time.
        // Each piece it hands out lies within one chunk, so a place in it is a place in the file.
        class Idat {

        public:
            Idat(const FileReader &file, const std::uint64_t at, const std::uint32_t left)
                : _file(&file), _at(at), _left(left), _buffer(READ_BYTES) {}

            // The next bytes of the stream, empty at its end. It ends where they end in at() and left().
            std::span<const std::uint8_t> next() {
                while (_left == 0) {
                    // Past the chunk's CRC to the next one's length and type.
                    if (fill(_at, CRC + CHUNK_HEAD) < CRC + CHUNK_HEAD) {
                        return {};
                    }

                    const std::uint8_t *head = _buffer.data() + (_at - _start) + CRC;

                    if (!named(std::span(head + TYPE, TYPE), "IDAT")) {
                        return {};
                    }

                    _left = big32(head);
                    _at += CRC + CHUNK_HEAD;
                }

                const std::size_t got = fill(_at, _left);

                if (got == 0) {
                    return {};
                }

                const std::span<const std::uint8_t> piece(_buffer.data() + (_at - _start), got);

                _at += piece.size();
                _left -= static_cast<std::uint32_t>(piece.size());

                return piece;
            }

            [[nodiscard]] std::uint64_t at() const { return _at; }
            [[nodiscard]] std::uint32_t left() const { return _left; }

        private:
            // Makes the buffer hold what lies from the offset, up to bytes of it, reading afresh when it
            // stops short but the file does not. Returns how much it holds.
            std::size_t fill(const std::uint64_t offset, const std::size_t bytes) {
                const bool outside = offset < _start || offset >= _start + _held;
                const bool shortOf = offset + bytes > _start + _held && _held == _buffer.size();

                if (outside || shortOf) {
                    _start = offset;
                    _held = _file->read(offset, _buffer.data(), _buffer.size());
                }

                return std::min<std::size_t>(bytes, static_cast<std::size_t>(_start + _held - offset));
            }

            const FileReader *_file;
            std::uint64_t _at;
            std::uint32_t _left;
            std::vector<std::uint8_t> _buffer;
            std::uint64_t _start = 0;
            std::size_t _held = 0;
        };

        // A raw deflate stream, the zlib wrapper being behind it.
        struct Inflater {
            z_stream stream{};
            bool ready;

            Inflater() : ready(inflateInit2(&stream, RAW_DEFLATE) == Z_OK) {}
            ~Inflater() {
                if (ready) {
                    inflateEnd(&stream);
                }
            }

            Inflater(const Inflater &) = delete;
            Inflater(Inflater &&) = delete;
            Inflater &operator=(const Inflater &) = delete;
            Inflater &operator=(Inflater &&) = delete;

            void feed(const std::span<const std::uint8_t> piece) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib reads next_in without writing it.
                stream.next_in = const_cast<std::uint8_t *>(piece.data());
                stream.avail_in = static_cast<uInt>(piece.size());
            }

            // Inflates into out. Returns what it made, or false on corrupt data.
            bool run(std::vector<std::uint8_t> &out, const int flush, std::size_t *made, bool *ended) {
                stream.next_out = out.data();
                stream.avail_out = static_cast<uInt>(out.size());

                const int result = inflate(&stream, flush);

                *made = out.size() - stream.avail_out;
                *ended = result == Z_STREAM_END;

                return result == Z_OK || result == Z_STREAM_END;
            }

            // A deflate block ends here and another follows.
            [[nodiscard]] bool between_blocks() const {
                constexpr int BLOCK_END = 128;
                constexpr int LAST_BLOCK = 64;

                return (stream.data_type & BLOCK_END) != 0 && (stream.data_type & LAST_BLOCK) == 0;
            }
        };

        [[nodiscard]] std::uint8_t paeth(const int left, const int up, const int corner) {
            const int guess = left + up - corner;
            const int toLeft = std::abs(guess - left);
            const int toUp = std::abs(guess - up);
            const int toCorner = std::abs(guess - corner);

            if (toLeft <= toUp && toLeft <= toCorner) {
                return static_cast<std::uint8_t>(left);
            }

            return static_cast<std::uint8_t>(toUp <= toCorner ? up : corner);
        }

#if defined(__x86_64__) || defined(_M_X64)
        // Paeth on whole pixels of 3 to 8 bytes at once: each still waits on the one to its left, but
        // its channels go together in 16 bit lanes, ties going left, then up, then to the corner.
        // As libpng's filter_sse2_intrinsics.c does it (libpng licence):
        // https://github.com/pnggroup/libpng/blob/libpng16/intel/filter_sse2_intrinsics.c
        TIV_AVX2 void paeth_pixels(const std::uint8_t *in, const std::uint8_t *above, std::uint8_t *row,
                                   const std::size_t bytes, const std::size_t step) {
            const __m128i zero = _mm_setzero_si128();
            const __m128i low = _mm_set1_epi16(0x00FF);
            __m128i left = zero;
            __m128i corner = zero;

            for (std::size_t i = 0; i + step <= bytes; i += step) {
                std::uint64_t up = 0;
                std::uint64_t filtered = 0;

                std::memcpy(&up, above + i, step);
                std::memcpy(&filtered, in + i, step);

                const __m128i b = _mm_unpacklo_epi8(_mm_cvtsi64_si128(static_cast<long long>(up)), zero);
                const __m128i x = _mm_unpacklo_epi8(_mm_cvtsi64_si128(static_cast<long long>(filtered)), zero);
                const __m128i toLeft = _mm_abs_epi16(_mm_sub_epi16(b, corner));
                const __m128i toUp = _mm_abs_epi16(_mm_sub_epi16(left, corner));
                const __m128i toCorner =
                        _mm_abs_epi16(_mm_add_epi16(_mm_sub_epi16(b, corner), _mm_sub_epi16(left, corner)));
                const __m128i smallest = _mm_min_epi16(toCorner, _mm_min_epi16(toLeft, toUp));
                const __m128i upOrCorner = _mm_blendv_epi8(corner, b, _mm_cmpeq_epi16(smallest, toUp));
                const __m128i guess = _mm_blendv_epi8(upOrCorner, left, _mm_cmpeq_epi16(smallest, toLeft));

                left = _mm_and_si128(_mm_add_epi16(x, guess), low);

                const auto out = static_cast<std::uint64_t>(_mm_cvtsi128_si64(_mm_packus_epi16(left, zero)));

                std::memcpy(row + i, &out, step);
                corner = b;
            }
        }
#endif

        // PNG's filters undone: https://www.w3.org/TR/png-3/#9Filter-types
        void unfilter(const std::uint8_t filter, const std::uint8_t *in, const std::uint8_t *above, std::uint8_t *row,
                      const std::size_t bytes, const std::size_t step) {
            constexpr std::uint8_t SUB = 1;
            constexpr std::uint8_t UP = 2;
            constexpr std::uint8_t AVERAGE = 3;
            constexpr std::uint8_t PAETH = 4;
            const std::size_t first = std::min(step, bytes);

            switch (filter) {
                case SUB:
                    std::memcpy(row, in, first);

                    for (std::size_t i = step; i < bytes; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + row[i - step]);
                    }

                    break;
                case UP:
                    for (std::size_t i = 0; i < bytes; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + above[i]);
                    }

                    break;
                case AVERAGE:
                    for (std::size_t i = 0; i < first; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + (above[i] / 2));
                    }

                    for (std::size_t i = step; i < bytes; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + ((row[i - step] + above[i]) / 2));
                    }

                    break;
                case PAETH:
#if defined(__x86_64__) || defined(_M_X64)
                    if (step >= 3 && step <= sizeof(std::uint64_t) && Simd::avx2()) {
                        paeth_pixels(in, above, row, bytes, step);

                        break;
                    }
#endif

                    for (std::size_t i = 0; i < first; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + above[i]);
                    }

                    for (std::size_t i = step; i < bytes; ++i) {
                        row[i] = static_cast<std::uint8_t>(in[i] + paeth(row[i - step], above[i], above[i - step]));
                    }

                    break;
                default:
                    std::memcpy(row, in, bytes);
                    break;
            }
        }

        // libpng's png_set_scale_16, which tinyiv's PNG decoder asks for: the nearest of 255 steps.
        [[nodiscard]] std::uint8_t scale16(const std::uint8_t high, const std::uint8_t low) {
            int value = high;

            value += ((static_cast<int>(low) - value + 128) * 65535) >> 24;

            return static_cast<std::uint8_t>(value);
        }

        [[nodiscard]] std::uint16_t word(const std::uint8_t *at) {
            return static_cast<std::uint16_t>((at[0] << 8U) | at[1]);
        }
    }

    struct PngBands::Lines {
        // Where a row goes as RGBA, or null to only unfilter it.
        using Target = std::function<std::uint8_t *(int y)>;
        // Told of each row once unfiltered, false to stop.
        using Done = std::function<bool(int y)>;

        const PngBands *png;
        std::vector<std::uint8_t> scanline;
        std::vector<std::uint8_t> above;
        std::vector<std::uint8_t> row;
        std::vector<std::uint16_t> wide;
        std::size_t filled = 0;
        // The next row to finish.
        int y;

        Lines(const PngBands &of, const int first, std::vector<std::uint8_t> before)
            : png(&of), scanline(of._rowBytes + 1), above(std::move(before)), row(of._rowBytes), y(first) {}

        // Takes inflated bytes up to the last row. False when told to stop.
        bool take(const std::span<const std::uint8_t> bytes, const int last, const Target &target, const Done &done) {
            for (std::size_t i = 0; i < bytes.size() && y < last;) {
                const std::size_t part = std::min(bytes.size() - i, scanline.size() - filled);

                std::memcpy(scanline.data() + filled, bytes.data() + i, part);
                filled += part;
                i += part;

                if (filled < scanline.size()) {
                    break;
                }

                unfilter(scanline.front(), scanline.data() + 1, above.data(), row.data(), png->_rowBytes,
                         static_cast<std::size_t>(png->_filterStep));

                if (std::uint8_t *out = target(y); out != nullptr) {
                    png->expand(row.data(), out, wide);
                }

                above.swap(row);
                filled = 0;
                ++y;

                if (!done(y - 1)) {
                    return false;
                }
            }

            return true;
        }
    };

    struct PngBands::Scan {
        PngBands *png;
        const Keep *keep;
        Inflater inflater;
        std::vector<std::uint8_t> window;
        std::vector<std::uint8_t> state;
        std::span<const std::uint8_t> piece;
        std::uint64_t pieceAt = 0;
        std::uint32_t pieceLeft = 0;
        // The last byte of the piece before, for a checkpoint at the start of this one.
        std::uint8_t lastByte = 0;
        std::uint64_t inflated = 0;
        std::uint64_t mark = CHECKPOINT_BYTES;
        // A checkpoint whose first row's row above is not whole yet.
        bool pending = false;

        Scan(PngBands &of, const Keep &kept) : png(&of), keep(&kept), window(WINDOW) {}

        void take_piece(const std::span<const std::uint8_t> next, Idat &idat) {
            if (!piece.empty()) {
                lastByte = piece.back();
            }

            piece = next;
            pieceAt = idat.at() - piece.size();
            pieceLeft = idat.left() + static_cast<std::uint32_t>(piece.size());
            inflater.feed(piece);
        }

        void remember(const std::span<const std::uint8_t> made) {
            std::uint8_t *ring = window.data();
            const std::uint8_t *bytes = made.data();

            for (std::size_t i = 0; i < made.size(); ++i) {
                ring[(inflated + i) % WINDOW] = bytes[i];
            }

            inflated += made.size();
        }

        // Keeps the last checkpoint's state, the row above its first being the one given.
        bool keep_last(const std::vector<std::uint8_t> &above) {
            std::memcpy(state.data() + (state.size() - png->_rowBytes), above.data(), png->_rowBytes);
            pending = false;

            return (*keep)(png->_checkpoints.size() - 1, state);
        }

        // A checkpoint here, where a block ends, once enough has been inflated since the last.
        bool checkpoint(const Lines &lines) {
            if (pending || inflated < mark || !inflater.between_blocks()) {
                return true;
            }

            const std::size_t line = png->_rowBytes + 1;
            const std::size_t used = piece.size() - inflater.stream.avail_in;
            const auto kept = static_cast<std::size_t>(std::min<std::uint64_t>(inflated, WINDOW));
            const Checkpoint point{
                    .file = pieceAt + used,
                    .left = pieceLeft - static_cast<std::uint32_t>(used),
                    .bits = static_cast<std::uint8_t>(inflater.stream.data_type & 7),
                    .prime = used > 0 ? piece[used - 1] : lastByte,
                    .out = inflated,
                    .first = static_cast<std::uint32_t>((inflated + line - 1) / line),
            };

            // The window is kept in order, oldest first, as inflateSetDictionary takes it.
            state.resize(kept + png->_rowBytes);

            const std::uint8_t *ring = window.data();
            std::uint8_t *ordered = state.data();

            for (std::size_t i = 0; i < kept; ++i) {
                ordered[i] = ring[(inflated - kept + i) % WINDOW];
            }

            png->_checkpoints.push_back(point);
            mark = inflated + CHECKPOINT_BYTES;
            pending = true;

            return !std::cmp_equal(point.first, lines.y) || keep_last(lines.above);
        }
    };

    void PngBands::take_chunk(const std::span<const std::uint8_t> type, const std::span<const std::uint8_t> body,
                              bool *plain) {
        constexpr std::size_t IHDR = 13;
        constexpr std::size_t DEPTH = 8;
        constexpr std::size_t COLOUR = 9;
        constexpr std::size_t INTERLACE = 12;
        constexpr std::uint32_t MAX_SIDE = std::uint32_t{1} << 30U;
        const std::uint8_t *data = body.data();

        if (named(type, "IHDR") && body.size() >= IHDR) {
            _width = static_cast<int>(std::min(big32(data), MAX_SIDE));
            _height = static_cast<int>(std::min(big32(data + 4), MAX_SIDE));
            _depth = data[DEPTH];
            _colour = data[COLOUR];
            *plain = data[INTERLACE] == 0;
        } else if (named(type, "PLTE")) {
            for (std::size_t i = 0; i + 2 < body.size() && i / 3 < _palette.size(); i += 3) {
                _palette.at(i / 3) = OPAQUE | data[i] | (static_cast<std::uint32_t>(data[i + 1]) << 8U)
                                     | (static_cast<std::uint32_t>(data[i + 2]) << 16U);
            }
        } else if (named(type, "tRNS") && colour_of(_colour) == Colour::Palette) {
            for (std::size_t i = 0; i < body.size() && i < _palette.size(); ++i) {
                _palette.at(i) = (_palette.at(i) & ~OPAQUE) | (static_cast<std::uint32_t>(data[i]) << 24U);
            }
        } else if (named(type, "tRNS") && body.size() >= 2) {
            _keyed = true;

            for (std::size_t i = 0; i < _key.size() && (i * 2) + 1 < body.size(); ++i) {
                _key.at(i) = word(data + (i * 2));
            }
        }
    }

    bool PngBands::supported(const std::span<const std::uint8_t> start, const bool plain) const {
        const bool depth = _depth == 8 || (_depth == 16 && colour_of(_colour) != Colour::Palette);
        const bool mappable = !Png::tone(start).hdr() || (colour_of(_colour) != Colour::Palette && !_keyed);

        return plain && samples_of(_colour) > 0 && depth && _width > 0 && _height > 0 && mappable;
    }

    std::unique_ptr<PngBands> PngBands::open(const std::filesystem::path &file, const Tone::Display &display) {
        const FileReader reader(file);
        std::array<std::uint8_t, SIGNATURE.size()> signature{};

        if (!reader.valid() || reader.read(0, signature.data(), signature.size()) != signature.size()
            || signature != SIGNATURE) {
            return nullptr;
        }

        std::unique_ptr<PngBands> held(new PngBands());
        // The chunks before the image data, as the whole file gives them to Png::tone().
        std::vector<std::uint8_t> start(SIGNATURE.begin(), SIGNATURE.end());
        std::uint64_t at = SIGNATURE.size();
        bool plain = false;

        held->_file = file;
        held->_palette.fill(OPAQUE);

        for (;;) {
            std::array<std::uint8_t, CHUNK_HEAD> chunk{};

            if (at > MAX_HEAD || reader.read(at, chunk.data(), chunk.size()) != chunk.size()) {
                return nullptr;
            }

            const std::uint32_t length = big32(chunk.data());
            const std::span<const std::uint8_t> type = std::span(chunk).subspan(4, TYPE);

            if (named(type, "IDAT")) {
                held->_data = at + CHUNK_HEAD;
                held->_length = length;

                break;
            }

            const std::size_t body = start.size() + CHUNK_HEAD;

            if (length > MAX_HEAD) {
                return nullptr;
            }

            // With its CRC, as Png::tone() walks the chunks as they lie in the file.
            start.insert(start.end(), chunk.begin(), chunk.end());
            start.resize(body + length + CRC);

            if (reader.read(at + CHUNK_HEAD, start.data() + body, length + CRC) != length + CRC) {
                return nullptr;
            }

            held->take_chunk(type, std::span(start).subspan(body, length), &plain);
            at += CHUNK_HEAD + length + CRC;
        }

        if (!held->supported(start, plain)) {
            return nullptr;
        }

        if (const Tone::Source tone = Png::tone(start); tone.hdr()) {
            held->_mapper.emplace(tone, display);
            held->_encoding = display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;
        }

        held->_filterStep = samples_of(held->_colour) * held->_depth / 8;
        held->_rowBytes = static_cast<std::size_t>(held->_width) * static_cast<std::size_t>(held->_filterStep);

        return held;
    }

    bool PngBands::alpha() const {
        const Colour colour = colour_of(_colour);

        return colour == Colour::GreyAlpha || colour == Colour::Rgba || _keyed
               || (colour == Colour::Palette
                   && std::ranges::any_of(_palette, [](const std::uint32_t c) { return c < OPAQUE; }));
    }

    void PngBands::expand_grey(const std::uint8_t *row, std::uint8_t *out) const {
        const bool wide = _depth == 16;
        const bool alpha = colour_of(_colour) == Colour::GreyAlpha;
        const auto step = static_cast<std::size_t>(_filterStep);
        const std::size_t half = wide ? 2 : 1;

        for (std::size_t x = 0; std::cmp_less(x, _width); ++x) {
            const std::uint8_t *in = row + (x * step);
            std::uint8_t *pixel = out + (x * Bitmap::CHANNELS);
            const std::uint8_t grey = wide ? scale16(in[0], in[1]) : in[0];
            const std::uint16_t raw = wide ? word(in) : in[0];

            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;

            if (alpha) {
                pixel[3] = wide ? scale16(in[half], in[half + 1]) : in[half];
            } else {
                pixel[3] = _keyed && raw == _key.at(0) ? CLEAR : SOLID;
            }
        }
    }

    void PngBands::expand_colour(const std::uint8_t *row, std::uint8_t *out) const {
        const bool wide = _depth == 16;
        const bool alpha = colour_of(_colour) == Colour::Rgba;
        const auto step = static_cast<std::size_t>(_filterStep);
        const std::size_t size = wide ? 2 : 1;

        for (std::size_t x = 0; std::cmp_less(x, _width); ++x) {
            const std::uint8_t *in = row + (x * step);
            std::uint8_t *pixel = out + (x * Bitmap::CHANNELS);

            for (std::size_t c = 0; c < (alpha ? 4U : 3U); ++c) {
                const std::uint8_t *sample = in + (c * size);

                pixel[c] = wide ? scale16(sample[0], sample[1]) : sample[0];
            }

            if (!alpha) {
                const bool keyed = _keyed && (wide ? word(in) : in[0]) == _key.at(0)
                                   && (wide ? word(in + 2) : in[1]) == _key.at(1)
                                   && (wide ? word(in + 4) : in[2]) == _key.at(2);

                pixel[3] = keyed ? CLEAR : SOLID;
            }
        }
    }

    void PngBands::expand_mapped(const Tone::Mapper &mapper, const std::uint8_t *row, std::uint8_t *out,
                                 std::vector<std::uint16_t> &wide) const {
        const int channels = samples_of(_colour);
        const std::size_t samples = static_cast<std::size_t>(_width) * static_cast<std::size_t>(channels);
        constexpr std::uint16_t WIDEN = 257;

        wide.resize(samples);

        // The samples as libvips gives them, 8 bit ones widened by 257 as Vips::prepare() does.
        std::uint16_t *to = wide.data();

        for (std::size_t i = 0; i < samples; ++i) {
            to[i] = _depth == 16 ? word(row + (i * 2)) : static_cast<std::uint16_t>(row[i] * WIDEN);
        }

        mapper.map(std::span<const std::uint16_t>(wide), channels,
                   std::span(out, static_cast<std::size_t>(_width) * Bitmap::CHANNELS));
    }

    void PngBands::expand(const std::uint8_t *row, std::uint8_t *out, std::vector<std::uint16_t> &wide) const {
        if (_mapper) {
            expand_mapped(*_mapper, row, out, wide);

            return;
        }

        // The common layouts as they are, without a pass pixel by pixel.
        if (_depth == 8 && !_keyed && colour_of(_colour) == Colour::Rgb) {
            Channels::expand(row, out, _width);

            return;
        }

        if (_depth == 8 && colour_of(_colour) == Colour::Rgba) {
            std::memcpy(out, row, _rowBytes);

            return;
        }

        switch (colour_of(_colour)) {
            case Colour::Palette:
                for (std::size_t x = 0; std::cmp_less(x, _width); ++x) {
                    std::memcpy(out + (x * Bitmap::CHANNELS), &_palette.at(row[x]), sizeof(std::uint32_t));
                }

                break;
            case Colour::Grey:
            case Colour::GreyAlpha:
                expand_grey(row, out);
                break;
            case Colour::Rgb:
            case Colour::Rgba:
                expand_colour(row, out);
                break;
        }
    }

    bool PngBands::scan(const int rows, const Take &take, const Keep &keep, const Abort *abort) {
        const FileReader reader(_file);
        Idat idat(reader, _data, _length);
        Scan scan(*this, keep);
        Lines lines(*this, 0, std::vector<std::uint8_t>(_rowBytes, 0));
        std::vector<std::uint8_t> out(INFLATE_BYTES);
        Bitmap band = Bitmap::allocate(_width, std::min(rows, _height), _encoding);
        int bandTop = 0;

        _checkpoints.clear();

        // The zlib wrapper's two bytes, then raw deflate.
        const std::span<const std::uint8_t> first = idat.next();

        if (!scan.inflater.ready || first.size() < ZLIB_WRAPPER) {
            return false;
        }

        scan.take_piece(first.subspan(ZLIB_WRAPPER), idat);
        _checkpoints.push_back(
                {.file = scan.pieceAt, .left = scan.pieceLeft, .bits = 0, .prime = 0, .out = 0, .first = 0});
        scan.state.assign(_rowBytes, 0);

        if (!keep(0, scan.state)) {
            return false;
        }

        const Lines::Target target = [&](const int y) { return band.row(y - bandTop).data(); };
        const Lines::Done done = [&](const int y) {
            if (scan.pending && std::cmp_equal(_checkpoints.back().first, y + 1) && !scan.keep_last(lines.above)) {
                return false;
            }

            if (y + 1 - bandTop < band.height() && y + 1 < _height) {
                return true;
            }

            const int count = y + 1 - bandTop;
            const bool taken =
                    !aborted(abort)
                    && take(bandTop, count, band.all().first(band.pitch() * static_cast<std::size_t>(count)));

            bandTop = y + 1;

            return taken;
        };

        for (bool ended = false; !ended && lines.y < _height;) {
            if (scan.inflater.stream.avail_in == 0) {
                const std::span<const std::uint8_t> next = idat.next();

                if (next.empty()) {
                    return false;
                }

                scan.take_piece(next, idat);
            }

            std::size_t made = 0;

            if (!scan.inflater.run(out, Z_BLOCK, &made, &ended)) {
                return false;
            }

            scan.remember(std::span(out).first(made));

            if (!lines.take(std::span(out).first(made), _height, target, done) || !scan.checkpoint(lines)) {
                return false;
            }
        }

        if (scan.pending) {
            _checkpoints.pop_back();
        }

        return lines.y == _height;
    }

    bool PngBands::decode_from(const std::size_t checkpoint, const int first, const int last, Bitmap *out,
                               const int outTop, const Recall &recall) const {
        const Checkpoint &point = _checkpoints.at(checkpoint);
        const FileReader reader(_file);
        Idat idat(reader, point.file, point.left);
        Inflater inflater;
        std::vector<std::uint8_t> state;

        if (!inflater.ready || !recall(checkpoint, state) || state.size() < _rowBytes) {
            return false;
        }

        const std::size_t kept = state.size() - _rowBytes;
        const bool primed =
                point.bits == 0 || inflatePrime(&inflater.stream, point.bits, point.prime >> (8U - point.bits)) == Z_OK;

        if (!primed
            || (kept > 0 && inflateSetDictionary(&inflater.stream, state.data(), static_cast<uInt>(kept)) != Z_OK)) {
            return false;
        }

        Lines lines(*this, static_cast<int>(point.first),
                    std::vector<std::uint8_t>(state.end() - static_cast<std::ptrdiff_t>(_rowBytes), state.end()));
        std::vector<std::uint8_t> buffer(INFLATE_BYTES);
        // The rest of the row the checkpoint fell in, which the row above already covers.
        std::uint64_t skip = (static_cast<std::uint64_t>(point.first) * (_rowBytes + 1)) - point.out;
        const Lines::Target target = [&](const int y) { return y >= first ? out->row(y - outTop).data() : nullptr; };
        const Lines::Done done = [](const int /*y*/) { return true; };

        for (bool ended = false; !ended && lines.y < last;) {
            if (inflater.stream.avail_in == 0) {
                const std::span<const std::uint8_t> piece = idat.next();

                if (piece.empty()) {
                    return false;
                }

                inflater.feed(piece);
            }

            std::size_t made = 0;

            if (!inflater.run(buffer, Z_NO_FLUSH, &made, &ended)) {
                return false;
            }

            const auto skipped = static_cast<std::size_t>(std::min<std::uint64_t>(skip, made));

            skip -= skipped;
            lines.take(std::span(buffer).subspan(skipped, made - skipped), last, target, done);
        }

        return lines.y >= last;
    }

    bool PngBands::decode(const int top, const int count, Bitmap *out, const int threads, const Recall &recall) const {
        const int bottom = std::min(top + count, _height);

        if (top < 0 || bottom <= top || out->width() != _width || out->height() < bottom - top
            || _checkpoints.empty()) {
            return false;
        }

        // Pieces begin at the checkpoints within the rows, the first at the last one before them.
        struct Piece {
            std::size_t checkpoint = 0;
            int first = 0;
            int last = 0;
        };

        std::vector<Piece> pieces;
        const auto after =
                std::ranges::upper_bound(_checkpoints, static_cast<std::uint32_t>(top), {}, &Checkpoint::first);
        auto at = static_cast<std::size_t>(std::max<std::ptrdiff_t>(after - _checkpoints.begin() - 1, 0));

        for (int from = top; from < bottom;) {
            std::size_t next = at + 1;

            // Too short a piece costs more to begin than it saves, so it runs on into the next.
            while (next < _checkpoints.size() && std::cmp_less(_checkpoints.at(next).first, from + MIN_ROWS)) {
                ++next;
            }

            const int to = next < _checkpoints.size() ? std::min(static_cast<int>(_checkpoints.at(next).first), bottom)
                                                      : bottom;

            pieces.push_back({.checkpoint = at, .first = from, .last = to});
            from = to;
            at = next;
        }

        std::atomic<std::size_t> taken = 0;
        std::atomic<bool> ok = true;

        const auto work = [&] {
            for (std::size_t i = taken++; i < pieces.size() && ok; i = taken++) {
                const Piece &piece = pieces.at(i);

                if (!decode_from(piece.checkpoint, piece.first, piece.last, out, top, recall)) {
                    ok = false;
                }
            }
        };

        {
            std::vector<std::jthread> workers;
            const auto spread = std::min(static_cast<std::size_t>(std::max(threads, 1)), pieces.size());

            for (std::size_t i = 1; i < spread; ++i) {
                workers.emplace_back(work);
            }

            work();
        }

        return ok;
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,portability-simd-intrinsics)
