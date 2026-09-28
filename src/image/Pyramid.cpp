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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "image/Bitmap.h"
#include "image/Pyramid.h"
#include "image/Simd.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics): the kernels walk rows with intrinsics.
namespace tiv {
    namespace {
        constexpr int MAX_THREADS = 8;
        constexpr int ROWS_PER_THREAD = 128;

        using RowKernel = void (*)(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, int pairs);

        inline std::uint8_t quarter(const int sum) {
            return static_cast<std::uint8_t>((static_cast<unsigned>(sum) + 2U) >> 2U);
        }

        // Pairs is how many output pixels have two source columns.
        void halve_row_scalar(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, const int pairs) {
            for (int x = 0; x < pairs; ++x) {
                const std::uint8_t *t = top + (static_cast<std::size_t>(x) * 8);
                const std::uint8_t *b = bottom + (static_cast<std::size_t>(x) * 8);
                std::uint8_t *o = out + (static_cast<std::size_t>(x) * 4);

                for (int c = 0; c < Bitmap::CHANNELS; ++c) {
                    o[c] = quarter(t[c] + t[c + 4] + b[c] + b[c + 4]);
                }
            }
        }

        std::uint32_t word_at(const std::uint8_t *row, const std::size_t pixel) {
            std::uint32_t word = 0;

            std::memcpy(&word, row + (pixel * 4), 4);

            return word;
        }

        // PQ words averaged channel by channel, the way RGBA8 bytes are.
        void halve_row_pq(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, const int pairs) {
            for (int x = 0; x < pairs; ++x) {
                const auto at = static_cast<std::size_t>(x) * 2;
                const std::array<std::uint32_t, 4> a = Bitmap::unpack(word_at(top, at));
                const std::array<std::uint32_t, 4> b = Bitmap::unpack(word_at(top, at + 1));
                const std::array<std::uint32_t, 4> c = Bitmap::unpack(word_at(bottom, at));
                const std::array<std::uint32_t, 4> d = Bitmap::unpack(word_at(bottom, at + 1));
                std::array<std::uint32_t, 4> sum{};

                for (std::size_t i = 0; i < sum.size(); ++i) {
                    sum.at(i) = (a.at(i) + b.at(i) + c.at(i) + d.at(i) + 2) >> 2U;
                }

                const auto [red, green, blue, alpha] = sum;
                const std::uint32_t word = Bitmap::pack(red, green, blue, alpha);

                std::memcpy(out + (static_cast<std::size_t>(x) * 4), &word, 4);
            }
        }

#if defined(__x86_64__) || defined(_M_X64)
        void halve_row_sse2(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, const int pairs) {
            const __m128i zero = _mm_setzero_si128();
            const __m128i two = _mm_set1_epi16(2);
            int x = 0;

            for (; x + 2 <= pairs; x += 2) {
                const __m128i t =
                        _mm_loadu_si128(reinterpret_cast<const __m128i *>(top + (static_cast<std::size_t>(x) * 8)));
                const __m128i b =
                        _mm_loadu_si128(reinterpret_cast<const __m128i *>(bottom + (static_cast<std::size_t>(x) * 8)));
                const __m128i s01 = _mm_add_epi16(_mm_unpacklo_epi8(t, zero), _mm_unpacklo_epi8(b, zero));
                const __m128i s23 = _mm_add_epi16(_mm_unpackhi_epi8(t, zero), _mm_unpackhi_epi8(b, zero));
                const __m128i a = _mm_add_epi16(s01, _mm_srli_si128(s01, 8));
                const __m128i c = _mm_add_epi16(s23, _mm_srli_si128(s23, 8));
                __m128i both = _mm_unpacklo_epi64(a, c);

                both = _mm_srli_epi16(_mm_add_epi16(both, two), 2);

                _mm_storel_epi64(reinterpret_cast<__m128i *>(out + (static_cast<std::size_t>(x) * 4)),
                                 _mm_packus_epi16(both, both));
            }

            halve_row_scalar(top + (static_cast<std::size_t>(x) * 8), bottom + (static_cast<std::size_t>(x) * 8),
                             out + (static_cast<std::size_t>(x) * 4), pairs - x);
        }

        TIV_AVX2 void halve_row_avx2(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out,
                                     const int pairs) {
            // Per lane: RGBA RGBA RGBA RGBA becomes RR GG BB AA RR GG BB AA, so one multiply-add
            // by one sums each channel of a pixel pair.
            // clang-format off
            const __m256i gather = _mm256_setr_epi8(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15,
                                                    0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);
            // clang-format on
            const __m256i ones = _mm256_set1_epi8(1);
            const __m256i two = _mm256_set1_epi16(2);
            int x = 0;

            for (; x + 8 <= pairs; x += 8) {
                const std::uint8_t *t = top + (static_cast<std::size_t>(x) * 8);
                const std::uint8_t *b = bottom + (static_cast<std::size_t>(x) * 8);
                const __m256i t0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(t));
                const __m256i t1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(t + 32));
                const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b));
                const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + 32));

                __m256i s0 = _mm256_add_epi16(_mm256_maddubs_epi16(_mm256_shuffle_epi8(t0, gather), ones),
                                              _mm256_maddubs_epi16(_mm256_shuffle_epi8(b0, gather), ones));
                __m256i s1 = _mm256_add_epi16(_mm256_maddubs_epi16(_mm256_shuffle_epi8(t1, gather), ones),
                                              _mm256_maddubs_epi16(_mm256_shuffle_epi8(b1, gather), ones));

                s0 = _mm256_srli_epi16(_mm256_add_epi16(s0, two), 2);
                s1 = _mm256_srli_epi16(_mm256_add_epi16(s1, two), 2);

                // packus interleaves the two by lane, the permute puts the pixels back in order.
                const __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi16(s0, s1), 0b11011000);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + (static_cast<std::size_t>(x) * 4)), packed);
            }

            halve_row_sse2(top + (static_cast<std::size_t>(x) * 8), bottom + (static_cast<std::size_t>(x) * 8),
                           out + (static_cast<std::size_t>(x) * 4), pairs - x);
        }

        // Each field of the words is pulled into lanes of its own, the rows added, and hadd adds
        // the pairs, leaving the lanes in the order 0 1 4 5 2 3 6 7, which the permute undoes.
        TIV_AVX2 void halve_row_pq_avx2(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out,
                                        const int pairs) {
            const __m256i ten = _mm256_set1_epi32(static_cast<int>(Bitmap::PQ_MASK));
            const __m256i two = _mm256_set1_epi32(2);
            int x = 0;

            for (; x + 8 <= pairs; x += 8) {
                const std::uint8_t *t = top + (static_cast<std::size_t>(x) * 8);
                const std::uint8_t *b = bottom + (static_cast<std::size_t>(x) * 8);
                const __m256i t0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(t));
                const __m256i t1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(t + 32));
                const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b));
                const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + 32));
                __m256i word = _mm256_setzero_si256();

                for (const int shift : {0, 10, 20, 30}) {
                    const __m256i count = _mm256_set1_epi64x(shift);
                    const __m256i field = shift == 30 ? _mm256_set1_epi32(-1) : ten;
                    const __m256i low = _mm256_add_epi32(
                            _mm256_and_si256(_mm256_srl_epi32(t0, _mm256_castsi256_si128(count)), field),
                            _mm256_and_si256(_mm256_srl_epi32(b0, _mm256_castsi256_si128(count)), field));
                    const __m256i high = _mm256_add_epi32(
                            _mm256_and_si256(_mm256_srl_epi32(t1, _mm256_castsi256_si128(count)), field),
                            _mm256_and_si256(_mm256_srl_epi32(b1, _mm256_castsi256_si128(count)), field));
                    const __m256i sums = _mm256_permute4x64_epi64(_mm256_hadd_epi32(low, high), 0b11011000);
                    const __m256i mean = _mm256_srli_epi32(_mm256_add_epi32(sums, two), 2);

                    word = _mm256_or_si256(word, _mm256_sll_epi32(mean, _mm256_castsi256_si128(count)));
                }

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + (static_cast<std::size_t>(x) * 4)), word);
            }

            halve_row_pq(top + (static_cast<std::size_t>(x) * 8), bottom + (static_cast<std::size_t>(x) * 8),
                         out + (static_cast<std::size_t>(x) * 4), pairs - x);
        }
#endif

        RowKernel pick(const Pyramid::Kernel kernel) {
#if defined(__x86_64__) || defined(_M_X64)
            switch (kernel) {
                case Pyramid::Kernel::Scalar:
                    return halve_row_scalar;
                case Pyramid::Kernel::Sse2:
                    return halve_row_sse2;
                case Pyramid::Kernel::Avx2:
                    return halve_row_avx2;
                case Pyramid::Kernel::Auto:
                    break;
            }

            return Simd::avx2() ? halve_row_avx2 : halve_row_sse2;
#else
            (void)kernel;

            return halve_row_scalar;
#endif
        }

        void halve_pair(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, const int width,
                        const RowKernel kernel, const Bitmap::Encoding encoding) {
            const int pairs = width / 2;

            kernel(top, bottom, out, pairs);

            if (width % 2 == 0) {
                return;
            }

            const std::size_t at = static_cast<std::size_t>(pairs) * 4;

            if (encoding == Bitmap::Encoding::Pq) {
                const auto [r0, g0, b0, a0] = Bitmap::unpack(word_at(top, static_cast<std::size_t>(pairs) * 2));
                const auto [r1, g1, b1, a1] = Bitmap::unpack(word_at(bottom, static_cast<std::size_t>(pairs) * 2));
                const std::uint32_t word = Bitmap::pack((r0 + r1 + 1) >> 1U, (g0 + g1 + 1) >> 1U, (b0 + b1 + 1) >> 1U,
                                                        (a0 + a1 + 1) >> 1U);

                std::memcpy(out + at, &word, 4);

                return;
            }

            for (int c = 0; c < Bitmap::CHANNELS; ++c) {
                out[at + c] = static_cast<std::uint8_t>((top[(at * 2) + c] + bottom[(at * 2) + c] + 1U) >> 1U);
            }
        }

        RowKernel pick(const Pyramid::Kernel kernel, const Bitmap::Encoding encoding) {
            if (encoding == Bitmap::Encoding::Srgb) {
                return pick(kernel);
            }

#if defined(__x86_64__) || defined(_M_X64)
            if (kernel != Pyramid::Kernel::Scalar && kernel != Pyramid::Kernel::Sse2 && Simd::avx2()) {
                return halve_row_pq_avx2;
            }
#endif

            return halve_row_pq;
        }

        void halve_rows(const Bitmap &source, Bitmap &target, const RowKernel kernel, const int from, const int to) {
            const int lastY = source.height() - 1;

            for (int y = from; y < to; ++y) {
                const std::uint8_t *top = source.row(std::min(2 * y, lastY)).data();
                const std::uint8_t *bottom = source.row(std::min((2 * y) + 1, lastY)).data();

                halve_pair(top, bottom, target.row(y).data(), source.width(), kernel, source.encoding());
            }
        }
    }

    void Pyramid::halve_row(const std::uint8_t *top, const std::uint8_t *bottom, std::uint8_t *out, const int width,
                            const Kernel kernel, const Bitmap::Encoding encoding) {
        halve_pair(top, bottom, out, width, pick(kernel, encoding), encoding);
    }

    Pyramid Pyramid::build(Bitmap base, const int thumbWidth, const int thumbHeight) {
        Pyramid held;

        if (base.empty()) {
            return held;
        }

        held.levels.push_back(std::make_shared<const Bitmap>(std::move(base)));

        while (exceeds(*held.levels.back(), thumbWidth, thumbHeight)) {
            held.levels.push_back(std::make_shared<const Bitmap>(halve(*held.levels.back())));
        }

        return held;
    }

    Bitmap Pyramid::halve(const Bitmap &source, const Kernel kernel) {
        Bitmap target = Bitmap::allocate((source.width() + 1) / 2, (source.height() + 1) / 2, source.encoding());
        const RowKernel rows = pick(kernel, source.encoding());

        const int wanted = std::min(MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
        const int threads = std::clamp(target.height() / ROWS_PER_THREAD, 1, std::max(wanted, 1));

        if (threads == 1) {
            halve_rows(source, target, rows, 0, target.height());

            return target;
        }

        std::vector<std::jthread> workers;
        const int band = (target.height() + threads - 1) / threads;

        for (int from = 0; from < target.height(); from += band) {
            workers.emplace_back(halve_rows, std::cref(source), std::ref(target), rows, from,
                                 std::min(from + band, target.height()));
        }

        workers.clear();

        return target;
    }

    bool Pyramid::supports(const Kernel kernel) {
#if defined(__x86_64__) || defined(_M_X64)
        switch (kernel) {
            case Kernel::Avx2:
                return Simd::avx2();
            case Kernel::Auto:
            case Kernel::Scalar:
            case Kernel::Sse2:
                return true;
        }

        return false;
#else
        return kernel == Kernel::Auto || kernel == Kernel::Scalar;
#endif
    }

    bool Pyramid::exceeds(const Bitmap &level, const int boxWidth, const int boxHeight) {
        return level.width() > boxWidth || level.height() > boxHeight;
    }

    std::size_t Pyramid::bytes() const {
        return std::accumulate(levels.begin(), levels.end(), std::size_t{0},
                               [](const std::size_t sum, const auto &level) { return sum + level->bytes(); });
    }

    std::size_t Pyramid::fitting(const int boxWidth, const int boxHeight) const {
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (!exceeds(*levels.at(i), boxWidth, boxHeight)) {
                return i;
            }
        }

        return levels.empty() ? 0 : levels.size() - 1;
    }

    Pyramid Pyramid::trimmed(const int boxWidth, const int boxHeight) const {
        Pyramid held;
        const std::span<const std::shared_ptr<const Bitmap>> kept =
                std::span(levels).subspan(fitting(boxWidth, boxHeight));

        held.levels.assign(kept.begin(), kept.end());

        return held;
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
