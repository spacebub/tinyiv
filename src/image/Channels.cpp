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
#include <cstdlib>

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

        void interleave_scalar(const std::uint8_t *red, const std::uint8_t *green, const std::uint8_t *blue,
                               const std::uint8_t *alpha, const bool greenApart, std::uint8_t *rgba,
                               const std::size_t pixels) {
            for (std::size_t x = 0; x < pixels; ++x) {
                const std::uint8_t added = greenApart ? green[x] : 0;
                std::uint8_t *out = rgba + (x * 4);

                out[0] = static_cast<std::uint8_t>(red[x] + added);
                out[1] = green[x];
                out[2] = static_cast<std::uint8_t>(blue[x] + added);
                out[3] = alpha != nullptr ? alpha[x] : 0xFF;
            }
        }

#if defined(__x86_64__) || defined(_M_X64)
        // Each byte of the 32 plus every one before it and the carry, which comes back as the last
        // byte in all 32. Log steps within each 128 bit lane, then the low lane's total into the high.
        TIV_AVX2 __m256i prefix_step(__m256i bytes, const __m256i carry) {
            bytes = _mm256_add_epi8(bytes, _mm256_slli_si256(bytes, 1));
            bytes = _mm256_add_epi8(bytes, _mm256_slli_si256(bytes, 2));
            bytes = _mm256_add_epi8(bytes, _mm256_slli_si256(bytes, 4));
            bytes = _mm256_add_epi8(bytes, _mm256_slli_si256(bytes, 8));

            const __m256i last = _mm256_shuffle_epi8(bytes, _mm256_set1_epi8(15));

            bytes = _mm256_add_epi8(bytes, _mm256_permute2x128_si256(last, last, 0x08));

            return _mm256_add_epi8(bytes, carry);
        }

        TIV_AVX2 std::size_t prefix_avx2(std::uint8_t *row, const std::size_t bytes) {
            __m256i carry = _mm256_setzero_si256();
            std::size_t i = 0;

            for (; i + 32 <= bytes; i += 32) {
                const __m256i summed =
                        prefix_step(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i)), carry);
                const __m256i last = _mm256_shuffle_epi8(summed, _mm256_set1_epi8(15));

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(row + i), summed);
                carry = _mm256_permute2x128_si256(last, last, 0x11);
            }

            return i;
        }

        // 32 pixels a step. The unpacks work within lanes, so the four results hold pixels 0 to 3
        // and 16 to 19, then 4 to 7 and 20 to 23, and so on, and the lanes are put back in order.
        TIV_AVX2 std::size_t interleave_avx2(const std::uint8_t *red, const std::uint8_t *green,
                                             const std::uint8_t *blue, const std::uint8_t *alpha, const bool greenApart,
                                             std::uint8_t *rgba, const std::size_t pixels) {
            std::size_t x = 0;

            for (; x + 32 <= pixels; x += 32) {
                const __m256i g = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(green + x));
                const __m256i added = greenApart ? g : _mm256_setzero_si256();
                const __m256i r =
                        _mm256_add_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(red + x)), added);
                const __m256i b =
                        _mm256_add_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(blue + x)), added);
                const __m256i a = alpha != nullptr ? _mm256_loadu_si256(reinterpret_cast<const __m256i *>(alpha + x))
                                                   : _mm256_set1_epi8(static_cast<char>(0xFF));
                const __m256i rgLow = _mm256_unpacklo_epi8(r, g);
                const __m256i rgHigh = _mm256_unpackhi_epi8(r, g);
                const __m256i baLow = _mm256_unpacklo_epi8(b, a);
                const __m256i baHigh = _mm256_unpackhi_epi8(b, a);
                const __m256i p0 = _mm256_unpacklo_epi16(rgLow, baLow);
                const __m256i p1 = _mm256_unpackhi_epi16(rgLow, baLow);
                const __m256i p2 = _mm256_unpacklo_epi16(rgHigh, baHigh);
                const __m256i p3 = _mm256_unpackhi_epi16(rgHigh, baHigh);
                auto *out = reinterpret_cast<__m256i *>(rgba + (x * 4));

                _mm256_storeu_si256(out, _mm256_permute2x128_si256(p0, p1, 0x20));
                _mm256_storeu_si256(out + 1, _mm256_permute2x128_si256(p2, p3, 0x20));
                _mm256_storeu_si256(out + 2, _mm256_permute2x128_si256(p0, p1, 0x31));
                _mm256_storeu_si256(out + 3, _mm256_permute2x128_si256(p2, p3, 0x31));
            }

            return x;
        }

        // Eight pixels a step. The 32 bytes read reach 8 past the 24 used, so the loop stops
        // while that much is left and the rest goes one pixel at a time.
        TIV_AVX2 void expand_avx2(const std::uint8_t *rgb, std::uint8_t *rgba, const int pixels) {
            // Bytes 0 to 11 into the low lane, 12 to 23 into the high one, four pixels each.
            const __m256i spread = _mm256_setr_epi32(0, 1, 2, 3, 3, 4, 5, 6);
            // clang-format off
            const __m256i place = _mm256_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
                                                   0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
            // clang-format on
            const __m256i opaque = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
            int x = 0;

            for (; x + 11 <= pixels; x += 8) {
                const __m256i in =
                        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(rgb + (static_cast<std::size_t>(x) * 3)));
                const __m256i out =
                        _mm256_or_si256(_mm256_shuffle_epi8(_mm256_permutevar8x32_epi32(in, spread), place), opaque);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(rgba + (static_cast<std::size_t>(x) * 4)), out);
            }

            expand_scalar(rgb + (static_cast<std::size_t>(x) * 3), rgba + (static_cast<std::size_t>(x) * 4),
                          pixels - x);
        }

        // Eight pixels a step. The 32 bytes written reach 8 past the 24 meant, which the next
        // step or the tail writes over, so the loop stops while that much is left.
        TIV_AVX2 void pack_avx2(const std::uint8_t *rgba, std::uint8_t *rgb, const int pixels) {
            // Per lane, four pixels become twelve bytes at the front.
            // clang-format off
            const __m256i squeeze = _mm256_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,
                                                     0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
            // clang-format on
            // The high lane's twelve bytes follow the low lane's.
            const __m256i join = _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 7, 7);
            int x = 0;

            for (; x + 11 <= pixels; x += 8) {
                const __m256i in =
                        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(rgba + (static_cast<std::size_t>(x) * 4)));
                const __m256i out = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(in, squeeze), join);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(rgb + (static_cast<std::size_t>(x) * 3)), out);
            }

            pack_scalar(rgba + (static_cast<std::size_t>(x) * 4), rgb + (static_cast<std::size_t>(x) * 3), pixels - x);
        }

        // Both return how many bytes they did, whole steps of 64, and leave the rest to the caller.
        TIV_AVX2 std::size_t difference_avx2(const std::uint8_t *row, const std::uint8_t *above, std::uint8_t *out,
                                             const std::size_t bytes) {
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

        // 32 pixels a step: each load's four pixels per lane are gathered into a dword a channel, the
        // four loads are unpacked by dword then by qword, which leaves each channel's pixels in the
        // order 0 to 3, 8 to 11, 16 to 19, 24 to 27 in the low lane and 4 to 7 onwards in the high
        // one, and the permute puts them in order.
        TIV_AVX2 std::size_t deinterleave_avx2(const std::uint8_t *rgba, const std::size_t pixels,
                                               const bool greenApart, std::uint8_t *red, std::uint8_t *green,
                                               std::uint8_t *blue, std::uint8_t *alpha) {
            // clang-format off
            const __m256i gather = _mm256_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15,
                                                    0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
            // clang-format on
            const __m256i order = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
            std::size_t x = 0;

            for (; x + 32 <= pixels; x += 32) {
                const auto *in = reinterpret_cast<const __m256i *>(rgba + (x * 4));
                const __m256i v0 = _mm256_shuffle_epi8(_mm256_loadu_si256(in), gather);
                const __m256i v1 = _mm256_shuffle_epi8(_mm256_loadu_si256(in + 1), gather);
                const __m256i v2 = _mm256_shuffle_epi8(_mm256_loadu_si256(in + 2), gather);
                const __m256i v3 = _mm256_shuffle_epi8(_mm256_loadu_si256(in + 3), gather);
                const __m256i rg01 = _mm256_unpacklo_epi32(v0, v1);
                const __m256i ba01 = _mm256_unpackhi_epi32(v0, v1);
                const __m256i rg23 = _mm256_unpacklo_epi32(v2, v3);
                const __m256i ba23 = _mm256_unpackhi_epi32(v2, v3);
                const __m256i g = _mm256_permutevar8x32_epi32(_mm256_unpackhi_epi64(rg01, rg23), order);
                const __m256i added = greenApart ? g : _mm256_setzero_si256();
                const __m256i r =
                        _mm256_sub_epi8(_mm256_permutevar8x32_epi32(_mm256_unpacklo_epi64(rg01, rg23), order), added);
                const __m256i b =
                        _mm256_sub_epi8(_mm256_permutevar8x32_epi32(_mm256_unpacklo_epi64(ba01, ba23), order), added);

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(red + x), r);
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(green + x), g);
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(blue + x), b);

                if (alpha != nullptr) {
                    _mm256_storeu_si256(reinterpret_cast<__m256i *>(alpha + x),
                                        _mm256_permutevar8x32_epi32(_mm256_unpackhi_epi64(ba01, ba23), order));
                }
            }

            return x;
        }

        // The 32 bytes moved one place right across the lanes, a zero coming in.
        TIV_AVX2 __m256i shift_right_byte(const __m256i bytes) {
            return _mm256_alignr_epi8(bytes, _mm256_permute2x128_si256(bytes, bytes, 0x08), 15);
        }

        TIV_AVX2 std::uint64_t total(const __m256i lanes) {
            const __m128i halves = _mm_add_epi64(_mm256_castsi256_si128(lanes), _mm256_extracti128_si256(lanes, 1));

            return static_cast<std::uint64_t>(_mm_cvtsi128_si64(halves))
                   + static_cast<std::uint64_t>(_mm_extract_epi64(halves, 1));
        }

        TIV_AVX2 std::size_t magnitudes_avx2(const std::uint8_t *row, const std::size_t bytes, std::uint64_t *plain,
                                             std::uint64_t *sloped) {
            const __m256i zero = _mm256_setzero_si256();
            __m256i flat = zero;
            __m256i slanted = zero;
            std::size_t i = 0;

            for (; i + 32 <= bytes; i += 32) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i));
                const __m256i before = i == 0 ? shift_right_byte(v)
                                              : _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i - 1));

                flat = _mm256_add_epi64(flat, _mm256_sad_epu8(_mm256_abs_epi8(v), zero));
                slanted = _mm256_add_epi64(slanted, _mm256_sad_epu8(_mm256_abs_epi8(_mm256_sub_epi8(v, before)), zero));
            }

            *plain += total(flat);
            *sloped += total(slanted);

            return i;
        }

        TIV_AVX2 std::size_t slope_avx2(std::uint8_t *row, const std::size_t bytes) {
            std::size_t i = 0;
            char carry = 0;

            for (; i + 32 <= bytes; i += 32) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i));
                const __m256i before = _mm256_insert_epi8(shift_right_byte(v), carry, 0);

                carry = static_cast<char>(_mm256_extract_epi8(v, 31));
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(row + i), _mm256_sub_epi8(v, before));
            }

            return i;
        }

        TIV_AVX2 std::size_t widen_add_avx2(const std::uint8_t *row, std::uint32_t *sums, const std::size_t bytes) {
            std::size_t i = 0;

            for (; i + 32 <= bytes; i += 32) {
                for (std::size_t part = 0; part < 32; part += 8) {
                    auto *at = reinterpret_cast<__m256i *>(sums + i + part);
                    const __m256i wide =
                            _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(row + i + part)));

                    _mm256_storeu_si256(at, _mm256_add_epi32(_mm256_loadu_si256(at), wide));
                }
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

    void Channels::difference(const std::uint8_t *row, const std::uint8_t *above, std::uint8_t *out,
                              const std::size_t bytes) {
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

    void Channels::prefix(std::uint8_t *row, const std::size_t bytes) {
        std::size_t i = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            i = prefix_avx2(row, bytes);
        }
#endif

        for (i = std::max<std::size_t>(i, 1); i < bytes; ++i) {
            row[i] = static_cast<std::uint8_t>(row[i] + row[i - 1]);
        }
    }

    void Channels::interleave(const std::uint8_t *red, const std::uint8_t *green, const std::uint8_t *blue,
                              const std::uint8_t *alpha, const bool greenApart, std::uint8_t *rgba,
                              const std::size_t pixels) {
        std::size_t x = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            x = interleave_avx2(red, green, blue, alpha, greenApart, rgba, pixels);
        }
#endif

        interleave_scalar(red + x, green + x, blue + x, alpha != nullptr ? alpha + x : nullptr, greenApart,
                          rgba + (x * 4), pixels - x);
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

    void Channels::deinterleave(const std::uint8_t *rgba, const std::size_t pixels, const bool greenApart,
                                std::uint8_t *red, std::uint8_t *green, std::uint8_t *blue, std::uint8_t *alpha) {
        std::size_t x = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            x = deinterleave_avx2(rgba, pixels, greenApart, red, green, blue, alpha);
        }
#endif

        for (; x < pixels; ++x) {
            const std::uint8_t *in = rgba + (x * 4);
            const std::uint8_t taken = greenApart ? in[1] : 0;

            red[x] = static_cast<std::uint8_t>(in[0] - taken);
            green[x] = in[1];
            blue[x] = static_cast<std::uint8_t>(in[2] - taken);

            if (alpha != nullptr) {
                alpha[x] = in[3];
            }
        }
    }

    void Channels::magnitudes(const std::uint8_t *row, const std::size_t bytes, std::uint64_t *plain,
                              std::uint64_t *sloped) {
        std::size_t i = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            i = magnitudes_avx2(row, bytes, plain, sloped);
        }
#endif

        for (; i < bytes; ++i) {
            const std::uint8_t before = i > 0 ? row[i - 1] : 0;

            *plain += static_cast<std::uint64_t>(std::abs(static_cast<int>(static_cast<std::int8_t>(row[i]))));
            *sloped += static_cast<std::uint64_t>(
                    std::abs(static_cast<int>(static_cast<std::int8_t>(static_cast<std::uint8_t>(row[i] - before)))));
        }
    }

    void Channels::slope(std::uint8_t *row, const std::size_t bytes) {
        std::size_t head = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            head = bytes - (bytes % 32);
        }
#endif

        // The tail first, right to left, while every byte before is still as it was.
        for (std::size_t x = bytes; x > std::max<std::size_t>(head, 1); --x) {
            row[x - 1] = static_cast<std::uint8_t>(row[x - 1] - row[x - 2]);
        }

#if defined(__x86_64__) || defined(_M_X64)
        if (head > 0) {
            slope_avx2(row, head);
        }
#endif
    }

    void Channels::widen_add(const std::uint8_t *row, std::uint32_t *sums, const std::size_t bytes) {
        std::size_t i = 0;

#if defined(__x86_64__) || defined(_M_X64)
        if (Simd::avx2()) {
            i = widen_add_avx2(row, sums, bytes);
        }
#endif

        for (; i < bytes; ++i) {
            sums[i] += row[i];
        }
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
