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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "image/Bitmap.h"
#include "image/Shrink.h"
#include "image/Simd.h"
#include "image/decode/Bmp.h"
#include "image/decode/Decode.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics): the kernels walk rows with intrinsics.
namespace tiv {
    namespace {
        using RowKernel = void (*)(const std::uint8_t *in, std::uint8_t *out, int width);

        constexpr std::size_t FILE_HEADER = 14;
        constexpr std::uint32_t CORE_HEADER = 12;
        constexpr std::uint32_t INFO_HEADER = 40;
        // BITMAPV2INFOHEADER adds the colour masks to the info header, V3 the alpha mask.
        constexpr std::uint32_t V2_HEADER = 52;
        constexpr std::uint32_t V3_HEADER = 56;
        constexpr std::uint32_t OS2_HEADER = 64;
        constexpr std::uint32_t OS2_SHORTEST = 16;

        constexpr std::uint32_t BI_RGB = 0;
        constexpr std::uint32_t BI_RLE8 = 1;
        constexpr std::uint32_t BI_RLE4 = 2;
        constexpr std::uint32_t BI_BITFIELDS = 3;
        constexpr std::uint32_t BI_ALPHABITFIELDS = 6;

        constexpr std::uint32_t RED = 0x00FF0000;
        constexpr std::uint32_t GREEN = 0x0000FF00;
        constexpr std::uint32_t BLUE = 0x000000FF;
        constexpr std::uint32_t ALPHA = 0xFF000000;

        // Keeps every offset and size product well inside 64 bits.
        constexpr std::int64_t MAX_SIDE = 1U << 20U;

        // Run length coded files are small palette images, and a larger claim is a broken header.
        constexpr std::int64_t MAX_RLE_PIXELS = 1U << 28U;

        // A target this large leaves the cache before anything reads it, so the kernels write
        // past the cache instead.
        constexpr std::size_t STREAM_BYTES = std::size_t{64} * 1024 * 1024;

        constexpr int MAX_THREADS = 8;
        constexpr int ROWS_PER_THREAD = 128;
        constexpr int ABORT_ROWS = 64;
        constexpr int ABORT_RUNS = 4096;

        std::uint32_t le16(const std::span<const std::uint8_t> data, const std::size_t at) {
            if (at > data.size() || data.size() - at < 2) {
                return 0;
            }

            return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8U);
        }

        std::uint32_t le32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return le16(data, at) | (le16(data, at + 2) << 16U);
        }

        bool aborted(const Decode::Abort *abort) {
            return abort != nullptr && abort->requested();
        }

        void bgr_scalar(const std::uint8_t *in, std::uint8_t *out, const int width) {
            for (int x = 0; x < width; ++x) {
                const std::uint8_t *from = in + (static_cast<std::size_t>(x) * 3);
                std::uint8_t *to = out + (static_cast<std::size_t>(x) * 4);

                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
                to[3] = 0xFF;
            }
        }

        template <bool Opaque>
        void bgra_scalar(const std::uint8_t *in, std::uint8_t *out, const int width) {
            for (int x = 0; x < width; ++x) {
                const std::uint8_t *from = in + (static_cast<std::size_t>(x) * 4);
                std::uint8_t *to = out + (static_cast<std::size_t>(x) * 4);

                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
                to[3] = Opaque ? 0xFF : from[3];
            }
        }

#if defined(__x86_64__) || defined(_M_X64)
        template <bool Stream>
        TIV_AVX2 void store_avx2(std::uint8_t *out, const __m256i pixels) {
            if constexpr (Stream) {
                _mm256_stream_si256(reinterpret_cast<__m256i *>(out), pixels);
            } else {
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out), pixels);
            }
        }

        // Pixels before the first 32 byte boundary of the row, which a streaming store needs.
        template <bool Stream>
        int unaligned(const std::uint8_t *out, const int width) {
            if constexpr (Stream) {
                const auto misplaced = static_cast<int>(reinterpret_cast<std::uintptr_t>(out) % 32);

                return std::min(misplaced == 0 ? 0 : (32 - misplaced) / 4, width);
            } else {
                return 0;
            }
        }

        template <bool Stream>
        TIV_AVX2 void bgr_avx2(const std::uint8_t *in, std::uint8_t *out, const int width) {
            // Each lane spreads four pixels from a load of its own, the alpha byte is ored in.
            // clang-format off
            const __m256i order = _mm256_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1, 8, 7, 6, -1, 11, 10, 9, -1,
                                                   2, 1, 0, -1, 5, 4, 3, -1, 8, 7, 6, -1, 11, 10, 9, -1);
            // clang-format on
            const __m256i opaque = _mm256_set1_epi32(static_cast<int>(ALPHA));
            int x = unaligned<Stream>(out, width);

            bgr_scalar(in, out, x);

            // The upper load runs four bytes past the eight pixels, into the two that follow.
            for (; x + 10 <= width; x += 8) {
                const std::uint8_t *from = in + (static_cast<std::size_t>(x) * 3);
                const __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i *>(from));
                const __m128i high = _mm_loadu_si128(reinterpret_cast<const __m128i *>(from + 12));
                const __m256i both = _mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1);

                store_avx2<Stream>(out + (static_cast<std::size_t>(x) * 4),
                                   _mm256_or_si256(_mm256_shuffle_epi8(both, order), opaque));
            }

            bgr_scalar(in + (static_cast<std::size_t>(x) * 3), out + (static_cast<std::size_t>(x) * 4), width - x);
        }

        template <bool Opaque, bool Stream>
        TIV_AVX2 void bgra_avx2(const std::uint8_t *in, std::uint8_t *out, const int width) {
            // clang-format off
            const __m256i order = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
                                                   2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
            // clang-format on
            const __m256i opaque = _mm256_set1_epi32(Opaque ? static_cast<int>(ALPHA) : 0);
            int x = unaligned<Stream>(out, width);

            bgra_scalar<Opaque>(in, out, x);

            for (; x + 8 <= width; x += 8) {
                const __m256i pixels =
                        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(in + (static_cast<std::size_t>(x) * 4)));

                store_avx2<Stream>(out + (static_cast<std::size_t>(x) * 4),
                                   _mm256_or_si256(_mm256_shuffle_epi8(pixels, order), opaque));
            }

            bgra_scalar<Opaque>(in + (static_cast<std::size_t>(x) * 4), out + (static_cast<std::size_t>(x) * 4),
                                width - x);
        }
#endif
    }

    struct Bmp::Image::Kernels {
        RowKernel bgr = nullptr;
        RowKernel bgra = nullptr;
        RowKernel bgrx = nullptr;
    };

    bool Bmp::supports(const Kernel kernel) {
#if defined(__x86_64__) || defined(_M_X64)
        return kernel != Kernel::Avx2 || Simd::avx2();
#else
        return kernel != Kernel::Avx2;
#endif
    }

    Bmp::Image::Kernels Bmp::Image::pick(const Kernel kernel, const bool stream) {
#if defined(__x86_64__) || defined(_M_X64)
        const bool avx2 = kernel == Kernel::Avx2 || (kernel == Kernel::Auto && Simd::avx2());

        if (avx2 && stream) {
            return {.bgr = bgr_avx2<true>, .bgra = bgra_avx2<false, true>, .bgrx = bgra_avx2<true, true>};
        }

        if (avx2) {
            return {.bgr = bgr_avx2<false>, .bgra = bgra_avx2<false, false>, .bgrx = bgra_avx2<true, false>};
        }
#else
        (void)kernel;
        (void)stream;
#endif

        return {.bgr = bgr_scalar, .bgra = bgra_scalar<false>, .bgrx = bgra_scalar<true>};
    }

    bool Bmp::Image::open(const std::span<const std::uint8_t> file, Image *out) {
        if (file.size() < FILE_HEADER || file[0] != 'B' || file[1] != 'M') {
            return false;
        }

        return out->read(file, FILE_HEADER, le32(file, 10), false);
    }

    bool Bmp::Image::open_icon(const std::span<const std::uint8_t> entry, Image *out) {
        return out->read(entry, 0, 0, true);
    }

    namespace {
        struct Fields {
            std::uint32_t size = 0;
            std::int64_t width = 0;
            std::int64_t height = 0;
            int depth = 0;
            std::uint32_t compression = BI_RGB;
            std::uint32_t used = 0;
        };

        // The header layouts follow https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-header-types
        // and the fields https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv5header.
        bool read_fields(const std::span<const std::uint8_t> data, const std::size_t header, Fields *out) {
            Fields fields;

            fields.size = le32(data, header);

            if (fields.size < CORE_HEADER || fields.size > data.size() - header) {
                return false;
            }

            if (fields.size == CORE_HEADER) {
                fields.width = le16(data, header + 4);
                fields.height = le16(data, header + 6);
                fields.depth = static_cast<int>(le16(data, header + 10));
            } else if (fields.size >= OS2_SHORTEST) {
                fields.width = static_cast<std::int32_t>(le32(data, header + 4));
                fields.height = static_cast<std::int32_t>(le32(data, header + 8));
                fields.depth = static_cast<int>(le16(data, header + 14));
                fields.compression = fields.size >= 20 ? le32(data, header + 16) : BI_RGB;
                fields.used = fields.size >= 36 ? le32(data, header + 32) : 0;
            } else {
                return false;
            }

            // OS/2 2.x numbers its Huffman and 24 bit run length schemes where Windows has bit fields.
            if ((fields.size == OS2_HEADER || fields.size < INFO_HEADER) && fields.compression >= BI_BITFIELDS) {
                return false;
            }

            *out = fields;

            return true;
        }

        // The colour table moves along when the masks take its start.
        std::array<std::uint32_t, 4> read_masks(const std::span<const std::uint8_t> data, const std::size_t header,
                                                const Fields &fields, std::size_t *palette) {
            if (fields.compression != BI_BITFIELDS && fields.compression != BI_ALPHABITFIELDS) {
                return {};
            }

            if (fields.size >= V2_HEADER) {
                return {
                        le32(data, header + 40),
                        le32(data, header + 44),
                        le32(data, header + 48),
                        fields.size >= V3_HEADER ? le32(data, header + 52) : 0,
                };
            }

            // A plain info header leaves the masks to the start of the colour table.
            const bool alpha = fields.compression == BI_ALPHABITFIELDS;
            const std::array<std::uint32_t, 4> masks = {
                    le32(data, *palette),
                    le32(data, *palette + 4),
                    le32(data, *palette + 8),
                    alpha ? le32(data, *palette + 12) : 0,
            };

            *palette += alpha ? 16 : 12;

            return masks;
        }

        std::uint32_t nibble(const std::uint8_t byte, const int i) {
            return static_cast<std::uint32_t>(i % 2 == 0 ? byte >> 4U : byte & 0x0FU);
        }

        // Gathers the pixels of one row at a time for a run length coded image, and hands each
        // row on as it ends, bottom row first. A row nothing was drawn in goes on empty.
        class RleRows {

        public:
            RleRows(const int width, const int height, const std::span<const std::uint8_t> palette,
                    const std::function<void(std::span<const std::uint8_t>)> &emit)
                : _palette(palette), _emit(&emit), _current(static_cast<std::size_t>(width) * Bitmap::CHANNELS),
                  _width(width), _height(height) {}

            [[nodiscard]] bool done() const { return _line >= _height; }

            void repeat(const int count, const std::uint8_t value, const bool nibbles) {
                for (int i = 0; i < count; ++i) {
                    put(nibbles ? nibble(value, i) : value);
                }
            }

            void literal(const std::span<const std::uint8_t> bytes, const int count, const bool nibbles) {
                for (int i = 0; i < count; ++i) {
                    put(nibbles ? nibble(bytes[static_cast<std::size_t>(i) / 2], i)
                                : bytes[static_cast<std::size_t>(i)]);
                }
            }

            void end_line() {
                advance();
                _x = 0;
            }

            void move(const int right, const int up) {
                _x = std::min(_x + right, _width);

                for (int i = 0; i < up && !done(); ++i) {
                    advance();
                }
            }

            // Every row still missing goes on empty.
            bool finish(const Decode::Abort *abort) {
                while (!done()) {
                    if (_line % ABORT_ROWS == 0 && aborted(abort)) {
                        return false;
                    }

                    advance();
                }

                return true;
            }

        private:
            void put(const std::uint32_t index) {
                if (_x < _width) {
                    std::copy_n(_palette.subspan(static_cast<std::size_t>(index) * 4, 4).begin(), 4,
                                _current.begin() + (static_cast<std::ptrdiff_t>(_x) * 4));
                    _drawn = true;
                    ++_x;
                }
            }

            void advance() {
                (*_emit)(_drawn ? std::span<const std::uint8_t>(_current) : std::span<const std::uint8_t>());

                if (_drawn) {
                    std::ranges::fill(_current, std::uint8_t{0});
                    _drawn = false;
                }

                ++_line;
            }

            std::span<const std::uint8_t> _palette;
            const std::function<void(std::span<const std::uint8_t>)> *_emit;
            std::vector<std::uint8_t> _current;
            int _width;
            int _height;
            int _x = 0;
            int _line = 0;
            bool _drawn = false;
        };
    }

    bool Bmp::Image::read(const std::span<const std::uint8_t> data, const std::size_t header, std::size_t pixelsAt,
                          const bool icon) {
        *this = Image{};

        Fields fields;

        if (!read_fields(data, header, &fields)) {
            return false;
        }

        // The height of an ICO entry counts the transparency mask below the pixels.
        const std::int64_t height = icon ? fields.height / 2 : fields.height;

        if (fields.width <= 0 || fields.width > MAX_SIDE || height == 0 || std::abs(height) > MAX_SIDE) {
            return false;
        }

        _bottomUp = height > 0;
        _width = static_cast<int>(fields.width);
        _height = static_cast<int>(std::abs(height));
        _depth = fields.depth;

        std::size_t palette = header + fields.size;
        std::array<std::uint32_t, 4> masks = read_masks(data, header, fields, &palette);
        bool alphaInPixels = false;

        if (!choose_layout(fields.compression, masks, alphaInPixels)) {
            return false;
        }

        const bool packed = _layout == Layout::Rle8 || _layout == Layout::Rle4;

        if (packed && (!_bottomUp || icon || fields.width * height > MAX_RLE_PIXELS)) {
            return false;
        }

        if (packed || _layout == Layout::Indexed) {
            const std::size_t end = icon || pixelsAt <= palette ? data.size() : std::min(pixelsAt, data.size());

            palette = read_palette(data, palette, end, fields.size == CORE_HEADER ? 3 : 4, fields.used);
        }

        if (icon || pixelsAt == 0 || pixelsAt >= data.size()) {
            pixelsAt = palette;
        }

        if (pixelsAt >= data.size()) {
            return false;
        }

        _pixels = data.subspan(pixelsAt);
        _stride = ((static_cast<std::size_t>(_width) * static_cast<std::size_t>(_depth)) + 31) / 32 * 4;

        if (icon) {
            find_mask();
        }

        set_masks(masks);

        // Alpha that is zero everywhere is taken for no alpha at all, as Chromium's reader does:
        // https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/platform/image-decoders/bmp/bmp_image_reader.cc
        if (alphaInPixels && any_alpha(masks.at(3))) {
            _alpha = Alpha::Pixels;
        } else if (!_mask.empty()) {
            _alpha = Alpha::Mask;
        } else {
            _alpha = Alpha::Opaque;
        }

        return true;
    }

    bool Bmp::Image::choose_layout(const std::uint32_t compression, std::array<std::uint32_t, 4> &masks,
                                   bool &alphaInPixels) {
        if (compression == BI_RLE8 || compression == BI_RLE4) {
            _layout = compression == BI_RLE8 ? Layout::Rle8 : Layout::Rle4;

            return _depth == (compression == BI_RLE8 ? 8 : 4);
        }

        if (compression == BI_BITFIELDS || compression == BI_ALPHABITFIELDS) {
            const bool standard = masks.at(0) == RED && masks.at(1) == GREEN && masks.at(2) == BLUE
                                  && (masks.at(3) == 0 || masks.at(3) == ALPHA);

            if (_depth == 16) {
                _layout = Layout::Masked16;
            } else {
                _layout = standard ? Layout::Bgra : Layout::Masked32;
            }

            alphaInPixels = masks.at(3) != 0;

            return _depth == 16 || _depth == 32;
        }

        if (compression != BI_RGB) {
            return false;
        }

        switch (_depth) {
            case 1:
            case 2:
            case 4:
            case 8:
                _layout = Layout::Indexed;
                return true;
            case 16:
                _layout = Layout::Masked16;
                masks = {0x7C00, 0x03E0, 0x001F, 0};
                return true;
            case 24:
                _layout = Layout::Bgr;
                return true;
            case 32:
                // The fourth byte is often left zero instead of opaque, which any_alpha() catches.
                _layout = Layout::Bgra;
                masks = {RED, GREEN, BLUE, ALPHA};
                alphaInPixels = true;
                return true;
            default:
                return false;
        }
    }

    std::size_t Bmp::Image::read_palette(const std::span<const std::uint8_t> data, const std::size_t palette,
                                         const std::size_t end, const std::size_t entry, const std::uint32_t used) {
        const std::size_t most = std::size_t{1} << static_cast<unsigned>(_depth);
        std::size_t count = used != 0 && used < most ? used : most;

        count = palette < end ? std::min(count, (end - palette) / entry) : 0;

        // Indices past the table show as opaque black.
        for (std::size_t i = 0; i < _palette.size(); i += 4) {
            _palette.at(i + 3) = 0xFF;
        }

        for (std::size_t i = 0; i < count; ++i) {
            const std::span<const std::uint8_t> bgr = data.subspan(palette + (i * entry), 3);

            _palette.at(i * 4) = bgr[2];
            _palette.at((i * 4) + 1) = bgr[1];
            _palette.at((i * 4) + 2) = bgr[0];
        }

        return palette + (count * entry);
    }

    void Bmp::Image::find_mask() {
        const std::size_t maskAt = _stride * static_cast<std::size_t>(_height);

        _maskStride = (static_cast<std::size_t>(_width) + 31) / 32 * 4;

        if (maskAt <= _pixels.size() && _maskStride * static_cast<std::size_t>(_height) <= _pixels.size() - maskAt) {
            _mask = _pixels.subspan(maskAt, _maskStride * static_cast<std::size_t>(_height));
        }
    }

    void Bmp::Image::set_masks(const std::array<std::uint32_t, 4> &masks) {
        for (std::size_t c = 0; c < masks.size(); ++c) {
            Channel &channel = _channels.at(c);
            const std::uint32_t mask = masks.at(c);

            channel = {};

            if (mask == 0) {
                continue;
            }

            channel.shift = static_cast<std::uint32_t>(std::countr_zero(mask));

            auto bits = static_cast<std::uint32_t>(std::bit_width(mask >> channel.shift));

            // Only the top eight bits of a wider channel matter.
            if (bits > 8) {
                channel.shift += bits - 8;
                bits = 8;
            }

            channel.low = (1U << bits) - 1;

            for (std::uint32_t value = 0; value <= channel.low; ++value) {
                channel.scale.at(value) = static_cast<std::uint8_t>(((value * 255) + (channel.low / 2)) / channel.low);
            }
        }
    }

    bool Bmp::Image::any_alpha(const std::uint32_t mask) const {
        const bool wide = _depth == 32;

        for (int y = 0; y < _height; ++y) {
            const std::span<const std::uint8_t> in = stored(y);
            std::uint32_t seen = 0;

            if (in.empty()) {
                continue;
            }

            for (int x = 0; x < _width; ++x) {
                seen |= (wide ? le32(in, static_cast<std::size_t>(x) * 4) : le16(in, static_cast<std::size_t>(x) * 2))
                        & mask;
            }

            if (seen != 0) {
                return true;
            }
        }

        return false;
    }

    // Row y from the top as the file stores it, or empty when the file ends before it does.
    std::span<const std::uint8_t> Bmp::Image::stored(const int y) const {
        const std::size_t at = static_cast<std::size_t>(_bottomUp ? _height - 1 - y : y) * _stride;
        const std::size_t needed = ((static_cast<std::size_t>(_width) * static_cast<std::size_t>(_depth)) + 7) / 8;

        if (at > _pixels.size() || _pixels.size() - at < needed) {
            return {};
        }

        return _pixels.subspan(at, std::min(_stride, _pixels.size() - at));
    }

    void Bmp::Image::row(const int y, const std::span<std::uint8_t> out, const Kernels &kernels) const {
        const std::span<const std::uint8_t> in = stored(y);

        if (in.empty()) {
            std::ranges::fill(out, std::uint8_t{0});

            return;
        }

        switch (_layout) {
            case Layout::Indexed:
                indexed_row(in, out);
                break;
            case Layout::Bgr:
                kernels.bgr(in.data(), out.data(), _width);
                break;
            case Layout::Bgra:
                (_alpha == Alpha::Pixels ? kernels.bgra : kernels.bgrx)(in.data(), out.data(), _width);
                break;
            case Layout::Masked16:
            case Layout::Masked32:
                masked_row(in, out);
                break;
            case Layout::Rle8:
            case Layout::Rle4:
                break;
        }

        // Set bits in the mask of an ICO entry are the transparent pixels, per
        // https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)
        if (_alpha == Alpha::Mask) {
            const std::span<const std::uint8_t> bits =
                    _mask.subspan(static_cast<std::size_t>(_bottomUp ? _height - 1 - y : y) * _maskStride, _maskStride);

            for (int x = 0; x < _width; ++x) {
                const auto bit = static_cast<std::size_t>(x);
                const bool clear = ((static_cast<unsigned>(bits[bit / 8]) >> (7U - (bit % 8U))) & 1U) != 0;

                out[(static_cast<std::size_t>(x) * 4) + 3] = clear ? 0 : 0xFF;
            }
        }
    }

    void Bmp::Image::indexed_row(const std::span<const std::uint8_t> in, const std::span<std::uint8_t> out) const {
        const auto depth = static_cast<std::size_t>(_depth);
        const std::uint32_t low = (1U << depth) - 1;

        for (int x = 0; x < _width; ++x) {
            const std::size_t bit = static_cast<std::size_t>(x) * depth;
            const std::uint32_t index = (static_cast<std::uint32_t>(in[bit / 8]) >> (8 - depth - (bit % 8))) & low;

            std::ranges::copy(std::span(_palette).subspan(static_cast<std::size_t>(index) * 4, 4),
                              out.subspan(static_cast<std::size_t>(x) * 4, 4).begin());
        }
    }

    void Bmp::Image::masked_row(const std::span<const std::uint8_t> in, const std::span<std::uint8_t> out) const {
        const bool wide = _layout == Layout::Masked32;
        const bool alpha = _alpha == Alpha::Pixels;
        const Channel &opacity = _channels.at(3);

        for (int x = 0; x < _width; ++x) {
            const std::uint32_t pixel =
                    wide ? le32(in, static_cast<std::size_t>(x) * 4) : le16(in, static_cast<std::size_t>(x) * 2);
            const std::span<std::uint8_t> to = out.subspan(static_cast<std::size_t>(x) * 4, 4);

            for (std::size_t c = 0; c < 3; ++c) {
                const Channel &channel = _channels.at(c);

                to[c] = channel.scale.at((pixel >> channel.shift) & channel.low);
            }

            to[3] = alpha ? opacity.scale.at((pixel >> opacity.shift) & opacity.low) : 0xFF;
        }
    }

    // The run length schemes of https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-compression.
    // Pixels a run skips stay transparent, as Chromium's reader leaves them.
    bool Bmp::Image::unpack_rle(const std::function<void(std::span<const std::uint8_t>)> &emit,
                                const Decode::Abort *abort) const {
        const std::span<const std::uint8_t> data = _pixels;
        const bool nibbles = _layout == Layout::Rle4;
        RleRows rows(_width, _height, _palette, emit);
        std::size_t at = 0;
        int runs = 0;

        while (!rows.done() && data.size() - at >= 2) {
            if (++runs % ABORT_RUNS == 0 && aborted(abort)) {
                return false;
            }

            const std::uint8_t count = data[at];
            const std::uint8_t value = data[at + 1];

            at += 2;

            if (count > 0) {
                rows.repeat(count, value, nibbles);
            } else if (value == 0) {
                rows.end_line();
            } else if (value == 1) {
                break;
            } else if (value == 2) {
                if (data.size() - at < 2) {
                    break;
                }

                rows.move(data[at], data[at + 1]);
                at += 2;
            } else {
                const std::size_t bytes = nibbles ? (value + 1U) / 2 : value;

                if (data.size() - at < bytes) {
                    break;
                }

                rows.literal(data.subspan(at, bytes), value, nibbles);

                // A literal run is padded to a 16 bit boundary.
                at = std::min(at + bytes + (bytes % 2), data.size());
            }
        }

        return rows.finish(abort);
    }

    // Runs cross rows, so they decode on one thread.
    bool Bmp::Image::decode_rle(const int factor, Bitmap &target, const Decode::Abort *abort) const {
        BoxShrink shrink = BoxShrink::upward(_width, _height, factor, &target);
        int y = _height;

        const auto emit = [&](const std::span<const std::uint8_t> row) {
            --y;

            if (factor > 1 && row.empty()) {
                shrink.skip();
            } else if (factor > 1) {
                shrink.push(row);
            } else if (row.empty()) {
                std::ranges::fill(target.row(y), std::uint8_t{0});
            } else {
                std::ranges::copy(row, target.row(y).begin());
            }
        };

        return unpack_rle(emit, abort);
    }

    // A band is rows of the target, so the bands of a shrink never share a block.
    void Bmp::Image::decode_band(const int from, const int to, const int factor, Bitmap &target, const Kernels &kernels,
                                 const Decode::Abort *abort, std::atomic<bool> &stopped) const {
        const auto halted = [&](const int done) {
            if (done % ABORT_ROWS == 0 && aborted(abort)) {
                stopped = true;
            }

            return stopped.load(std::memory_order_relaxed);
        };

        if (factor == 1) {
            for (int y = from; y < to && !halted(y - from); ++y) {
                row(y, target.row(y), kernels);
            }

#if defined(__x86_64__) || defined(_M_X64)
            // Streaming stores are weakly ordered, so each thread fences its own.
            _mm_sfence();
#endif

            return;
        }

        std::vector<std::uint8_t> line(static_cast<std::size_t>(_width) * Bitmap::CHANNELS);
        BoxShrink shrink(_width, _height, factor, &target, from * factor);
        const int last = std::min(to * factor, _height);

        // Rows a truncated file lacks cost nothing, however many the header claims.
        for (int y = from * factor; y < last && !halted(y - (from * factor)); ++y) {
            if (stored(y).empty()) {
                shrink.skip();
            } else {
                row(y, line, kernels);
                shrink.push(line);
            }
        }
    }

    bool Bmp::Image::decode(const int factor, Bitmap *out, const Decode::Abort *abort, const Kernel kernel) const {
        Bitmap target = Bitmap::allocate((_width + factor - 1) / factor, (_height + factor - 1) / factor);

        if (_layout == Layout::Rle8 || _layout == Layout::Rle4) {
            if (!decode_rle(factor, target, abort)) {
                return false;
            }

            *out = std::move(target);

            return true;
        }

        const Kernels kernels = pick(kernel, factor == 1 && target.bytes() >= STREAM_BYTES);
        const int wanted = std::min(MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
        const int threads = std::clamp(_height / ROWS_PER_THREAD, 1, std::max(wanted, 1));
        const int band = (target.height() + threads - 1) / threads;
        std::atomic<bool> stopped = false;

        {
            std::vector<std::jthread> workers;

            for (int from = band; from < target.height(); from += band) {
                workers.emplace_back([&, from] {
                    decode_band(from, std::min(from + band, target.height()), factor, target, kernels, abort, stopped);
                });
            }

            decode_band(0, std::min(band, target.height()), factor, target, kernels, abort, stopped);
        }

        if (stopped || aborted(abort)) {
            return false;
        }

        *out = std::move(target);

        return true;
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
