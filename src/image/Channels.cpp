// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "image/Channels.h"
#include "image/Simd.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics): the kernels walk rows with intrinsics.
namespace tiv {
    namespace {
        void expand_scalar(const std::uint8_t *rgb, std::uint8_t *rgba, const int pixels) {
            for (int x = 0; x < pixels; ++x) {
                const std::uint8_t *in = rgb + (static_cast<std::size_t>(x) * 3);
                std::uint8_t *out = rgba + (static_cast<std::size_t>(x) * 4);

                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out[3] = 0xFF;
            }
        }

        void pack_scalar(const std::uint8_t *rgba, std::uint8_t *rgb, const int pixels) {
            for (int x = 0; x < pixels; ++x) {
                const std::uint8_t *in = rgba + (static_cast<std::size_t>(x) * 4);
                std::uint8_t *out = rgb + (static_cast<std::size_t>(x) * 3);

                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
            }
        }

#if defined(__x86_64__) || defined(_M_X64)
        // Eight pixels a step. The 32 bytes read reach 8 past the 24 used, so the loop stops
        // while that much is left and the rest goes one pixel at a time.
        TIV_AVX2 void expand_avx2(const std::uint8_t *rgb, std::uint8_t *rgba, const int pixels) {
            // Bytes 0 to 11 into the low lane, 12 to 23 into the high one, four pixels each.
            const __m256i spread = _mm256_setr_epi32(0, 1, 2, 3, 3, 4, 5, 6);
            const __m256i place = _mm256_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
                                                   0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
            const __m256i opaque = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
            int x = 0;

            for (; x + 11 <= pixels; x += 8) {
                const __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(rgb + (static_cast<std::size_t>(x) * 3)));
                const __m256i out = _mm256_or_si256(_mm256_shuffle_epi8(_mm256_permutevar8x32_epi32(in, spread), place), opaque);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(rgba + (static_cast<std::size_t>(x) * 4)), out);
            }

            expand_scalar(rgb + (static_cast<std::size_t>(x) * 3), rgba + (static_cast<std::size_t>(x) * 4), pixels - x);
        }

        // Eight pixels a step. The 32 bytes written reach 8 past the 24 meant, which the next
        // step or the tail writes over, so the loop stops while that much is left.
        TIV_AVX2 void pack_avx2(const std::uint8_t *rgba, std::uint8_t *rgb, const int pixels) {
            // Per lane, four pixels become twelve bytes at the front.
            const __m256i squeeze = _mm256_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,
                                                     0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
            // The high lane's twelve bytes follow the low lane's.
            const __m256i join = _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 7, 7);
            int x = 0;

            for (; x + 11 <= pixels; x += 8) {
                const __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(rgba + (static_cast<std::size_t>(x) * 4)));
                const __m256i out = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(in, squeeze), join);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(rgb + (static_cast<std::size_t>(x) * 3)), out);
            }

            pack_scalar(rgba + (static_cast<std::size_t>(x) * 4), rgb + (static_cast<std::size_t>(x) * 3), pixels - x);
        }

        // Both return how many bytes they did, whole steps of 64, and leave the rest to the caller.
        TIV_AVX2 std::size_t difference_avx2(const std::uint8_t *row, const std::uint8_t *above, std::uint8_t *out, const std::size_t bytes) {
            std::size_t i = 0;

            for (; i + 64 <= bytes; i += 64) {
                const __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i));
                const __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i + 32));
                const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(above + i));
                const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(above + i + 32));

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i), _mm256_sub_epi8(r0, a0));
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i + 32), _mm256_sub_epi8(r1, a1));
            }

            return i;
        }

        TIV_AVX2 std::size_t accumulate_avx2(std::uint8_t *row, const std::uint8_t *above, const std::size_t bytes) {
            std::size_t i = 0;

            for (; i + 64 <= bytes; i += 64) {
                const __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i));
                const __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i + 32));
                const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(above + i));
                const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(above + i + 32));

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(row + i), _mm256_add_epi8(r0, a0));
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(row + i + 32), _mm256_add_epi8(r1, a1));
            }

            return i;
        }
#endif
    }

    void Channels::expand(const std::uint8_t *rgb, std::uint8_t *rgba, const int pixels) {
#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            expand_avx2(rgb, rgba, pixels);

            return;
        }
#endif

        expand_scalar(rgb, rgba, pixels);
    }

    void Channels::pack(const std::uint8_t *rgba, std::uint8_t *rgb, const int pixels) {
#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            pack_avx2(rgba, rgb, pixels);

            return;
        }
#endif

        pack_scalar(rgba, rgb, pixels);
    }

    void Channels::difference(const std::uint8_t *row, const std::uint8_t *above, std::uint8_t *out, const std::size_t bytes) {
        std::size_t i = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            i = difference_avx2(row, above, out, bytes);
        }
#endif

        for (; i < bytes; ++i) {
            out[i] = static_cast<std::uint8_t>(row[i] - above[i]);
        }
    }

    void Channels::accumulate(std::uint8_t *row, const std::uint8_t *above, const std::size_t bytes) {
        std::size_t i = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            i = accumulate_avx2(row, above, bytes);
        }
#endif

        for (; i < bytes; ++i) {
            row[i] = static_cast<std::uint8_t>(row[i] + above[i]);
        }
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
