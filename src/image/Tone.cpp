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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "image/Simd.h"
#include "image/Tone.h"

namespace tiv {
    namespace {
        // Where HDR puts SDR white: https://www.itu.int/pub/R-REP-BT.2408
        constexpr float SDR_WHITE_NITS = 203.0F;

        // HLG is shown as on the reference display of BT.2100, 1000 nits with a system gamma of 1.2.
        constexpr float HLG_PEAK_NITS = 1000.0F;
        constexpr float HLG_GAMMA = 1.2F;

        // Linear light up to this share of the headroom is left alone, the rest bends towards
        // the headroom without reaching it.
        constexpr float KNEE = 0.8F;

        constexpr std::size_t SAMPLES = 65536;
        constexpr std::size_t ENCODED = 16384;
        constexpr std::size_t CHUNK = 256;

        // The brightest PQ holds, relative to SDR white.
        constexpr float PQ_TOP = 10000.0F / SDR_WHITE_NITS;
        constexpr std::uint32_t TEN_BITS = 1023;

        // SMPTE ST 2084, in nits: https://www.itu.int/rec/R-REC-BT.2100
        float pq_nits(const float signal) {
            constexpr double M1 = 2610.0 / 16384.0;
            constexpr double M2 = 2523.0 / 4096.0 * 128.0;
            constexpr double C1 = 3424.0 / 4096.0;
            constexpr double C2 = 2413.0 / 4096.0 * 32.0;
            constexpr double C3 = 2392.0 / 4096.0 * 32.0;

            const double power = std::pow(std::clamp(static_cast<double>(signal), 0.0, 1.0), 1.0 / M2);

            return static_cast<float>(std::pow(std::max(power - C1, 0.0) / (C2 - (C3 * power)), 1.0 / M1) * 10000.0);
        }

        // The inverse OETF of BT.2100, scene light from 0 to 1.
        float hlg_scene(const float signal) {
            constexpr double A = 0.17883277;
            constexpr double B = 1.0 - (4.0 * A);
            const double C = 0.5 - (A * std::log(4.0 * A));
            const double e = std::clamp(static_cast<double>(signal), 0.0, 1.0);

            return static_cast<float>(e <= 0.5 ? e * e / 3.0 : (std::exp((e - C) / A) + B) / 12.0);
        }

        using Table = std::vector<float>;

        Table fill(float (*decode)(float)) {
            Table table(SAMPLES);

            for (std::size_t i = 0; i < SAMPLES; ++i) {
                table.at(i) = decode(static_cast<float>(i) / static_cast<float>(SAMPLES - 1));
            }

            return table;
        }

        float pq_relative(const float signal) {
            return pq_nits(signal) / SDR_WHITE_NITS;
        }

        float identity(const float signal) {
            return signal;
        }

        const Table &table_for(const Tone::Transfer transfer) {
            static const Table PQ = fill(pq_relative);
            static const Table HLG = fill(hlg_scene);
            static const Table LINEAR = fill(identity);

            switch (transfer) {
                case Tone::Transfer::Pq:
                    return PQ;
                case Tone::Transfer::Hlg:
                    return HLG;
                case Tone::Transfer::Sdr:
                case Tone::Transfer::Linear:
                    break;
            }

            return LINEAR;
        }

        float decode(const Tone::Transfer transfer, const float signal) {
            switch (transfer) {
                case Tone::Transfer::Pq:
                    return pq_relative(signal);
                case Tone::Transfer::Hlg:
                    return hlg_scene(signal);
                case Tone::Transfer::Sdr:
                case Tone::Transfer::Linear:
                    break;
            }

            return signal;
        }

        // A gather reads four bytes from where each entry starts, so the tables run on past their
        // last entry by what that reaches.
        using SrgbTable = std::array<std::uint8_t, ENCODED + 3>;
        using PqTable = std::array<std::uint16_t, ENCODED + 1>;

        // The sRGB curve of IEC 61966-2-1 over linear light from 0 to 1.
        const SrgbTable &srgb_table() {
            static const SrgbTable TABLE = [] {
                SrgbTable held{};

                for (std::size_t i = 0; i < ENCODED; ++i) {
                    const double v = static_cast<double>(i) / static_cast<double>(ENCODED - 1);
                    const double encoded = v <= 0.0031308 ? v * 12.92 : (1.055 * std::pow(v, 1.0 / 2.4)) - 0.055;

                    held.at(i) = static_cast<std::uint8_t>(std::lround(encoded * 255.0));
                }

                return held;
            }();

            return TABLE;
        }

        // The sRGB curve of IEC 61966-2-1 undone, for 8 bit samples.
        const std::array<float, 256> &srgb_linear() {
            static const std::array<float, 256> TABLE = [] {
                std::array<float, 256> held{};

                for (std::size_t i = 0; i < held.size(); ++i) {
                    const double v = static_cast<double>(i) / 255.0;

                    held.at(i) = static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
                }

                return held;
            }();

            return TABLE;
        }

        // SMPTE ST 2084 in 10 bits, over the square root of light relative to what PQ holds,
        // which spends the steps where the curve is steep, near black.
        const PqTable &pq_table() {
            static const PqTable TABLE = [] {
                constexpr double M1 = 2610.0 / 16384.0;
                constexpr double M2 = 2523.0 / 4096.0 * 128.0;
                constexpr double C1 = 3424.0 / 4096.0;
                constexpr double C2 = 2413.0 / 4096.0 * 32.0;
                constexpr double C3 = 2392.0 / 4096.0 * 32.0;
                PqTable held{};

                for (std::size_t i = 0; i < ENCODED; ++i) {
                    const double root = static_cast<double>(i) / static_cast<double>(ENCODED - 1);
                    const double power = std::pow(root * root, M1);
                    const double signal = std::pow((C1 + (C2 * power)) / (1.0 + (C3 * power)), M2);

                    held.at(i) = static_cast<std::uint16_t>(std::lround(signal * TEN_BITS));
                }

                return held;
            }();

            return TABLE;
        }

        using Matrix = Tone::Matrix;

        Matrix multiply(const Matrix &a, const Matrix &b) {
            Matrix out{};

            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        out.at((row * 3) + column) += a.at((row * 3) + k) * b.at((k * 3) + column);
                    }
                }
            }

            return out;
        }

        Matrix invert(const Matrix &m) {
            const auto at = [&](const std::size_t row, const std::size_t column) {
                return m.at((row * 3) + column);
            };

            const float det = (at(0, 0) * ((at(1, 1) * at(2, 2)) - (at(1, 2) * at(2, 1)))) - (at(0, 1) * ((at(1, 0) * at(2, 2)) - (at(1, 2) * at(2, 0))))
                              + (at(0, 2) * ((at(1, 0) * at(2, 1)) - (at(1, 1) * at(2, 0))));

            return {((at(1, 1) * at(2, 2)) - (at(1, 2) * at(2, 1))) / det, ((at(0, 2) * at(2, 1)) - (at(0, 1) * at(2, 2))) / det,
                    ((at(0, 1) * at(1, 2)) - (at(0, 2) * at(1, 1))) / det, ((at(1, 2) * at(2, 0)) - (at(1, 0) * at(2, 2))) / det,
                    ((at(0, 0) * at(2, 2)) - (at(0, 2) * at(2, 0))) / det, ((at(0, 2) * at(1, 0)) - (at(0, 0) * at(1, 2))) / det,
                    ((at(1, 0) * at(2, 1)) - (at(1, 1) * at(2, 0))) / det, ((at(0, 1) * at(2, 0)) - (at(0, 0) * at(2, 1))) / det,
                    ((at(0, 0) * at(1, 1)) - (at(0, 1) * at(1, 0))) / det};
        }

        // RGB to XYZ from the primaries and the D65 white they share:
        // http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
        Matrix to_xyz(const Tone::Primaries primaries) {
            struct Xy {
                float x = 0.0F;
                float y = 0.0F;
            };

            constexpr Xy WHITE{0.3127F, 0.3290F};
            std::array<Xy, 3> rgb{};

            switch (primaries) {
                case Tone::Primaries::Bt709:
                    rgb = {Xy{0.640F, 0.330F}, Xy{0.300F, 0.600F}, Xy{0.150F, 0.060F}};
                    break;
                case Tone::Primaries::P3:
                    rgb = {Xy{0.680F, 0.320F}, Xy{0.265F, 0.690F}, Xy{0.150F, 0.060F}};
                    break;
                case Tone::Primaries::Bt2020:
                    rgb = {Xy{0.708F, 0.292F}, Xy{0.170F, 0.797F}, Xy{0.131F, 0.046F}};
                    break;
            }

            Matrix columns{};

            for (std::size_t c = 0; c < 3; ++c) {
                columns.at(c) = rgb.at(c).x / rgb.at(c).y;
                columns.at(3 + c) = 1.0F;
                columns.at(6 + c) = (1.0F - rgb.at(c).x - rgb.at(c).y) / rgb.at(c).y;
            }

            const Matrix inverse = invert(columns);
            const std::array<float, 3> white{WHITE.x / WHITE.y, 1.0F, (1.0F - WHITE.x - WHITE.y) / WHITE.y};
            Matrix out = columns;

            for (std::size_t c = 0; c < 3; ++c) {
                const float scale = (inverse.at(c * 3) * white.at(0)) + (inverse.at((c * 3) + 1) * white.at(1)) + (inverse.at((c * 3) + 2) * white.at(2));

                for (std::size_t row = 0; row < 3; ++row) {
                    out.at((row * 3) + c) *= scale;
                }
            }

            return out;
        }

        void apply_matrix(const Matrix &m, std::span<float> rgba) {
            const auto &[m0, m1, m2, m3, m4, m5, m6, m7, m8] = m;

            for (std::size_t at = 0; at + 4 <= rgba.size(); at += 4) {
                const float r = rgba[at];
                const float g = rgba[at + 1];
                const float b = rgba[at + 2];

                rgba[at] = (m0 * r) + (m1 * g) + (m2 * b);
                rgba[at + 1] = (m3 * r) + (m4 * g) + (m5 * b);
                rgba[at + 2] = (m6 * r) + (m7 * g) + (m8 * b);
            }
        }

        // HLG's OOTF, which brightens by the luminance of the scene: BT.2100, table 5.
        void hlg_display(const Tone::Primaries primaries, std::span<float> rgba) {
            const Matrix xyz = to_xyz(primaries);
            const float kr = xyz.at(3);
            const float kg = xyz.at(4);
            const float kb = xyz.at(5);
            const float scale = HLG_PEAK_NITS / SDR_WHITE_NITS;

            for (std::size_t at = 0; at + 4 <= rgba.size(); at += 4) {
                const float luminance = (kr * rgba[at]) + (kg * rgba[at + 1]) + (kb * rgba[at + 2]);
                const float gain = luminance > 0.0F ? scale * std::pow(luminance, HLG_GAMMA - 1.0F) : 0.0F;

                rgba[at] *= gain;
                rgba[at + 1] *= gain;
                rgba[at + 2] *= gain;
            }
        }

        template <typename Sample>
        float alpha_of(const Sample sample) {
            if constexpr (std::is_same_v<Sample, std::uint16_t>) {
                return static_cast<float>(sample) / static_cast<float>(SAMPLES - 1);
            } else if constexpr (std::is_same_v<Sample, std::uint8_t>) {
                return static_cast<float>(sample) / 255.0F;
            } else {
                return sample;
            }
        }

        template <typename Sample, typename Decode>
        void spread(const std::span<const Sample> in, const int channels, std::span<float> out, const Decode &decode) {
            const auto step = static_cast<std::size_t>(channels);

            for (std::size_t from = 0, to = 0; from + step <= in.size() && to + 4 <= out.size(); from += step, to += 4) {
                if (channels < 3) {
                    const float grey = decode(in[from]);

                    out[to] = grey;
                    out[to + 1] = grey;
                    out[to + 2] = grey;
                } else {
                    out[to] = decode(in[from]);
                    out[to + 1] = decode(in[from + 1]);
                    out[to + 2] = decode(in[from + 2]);
                }

                out[to + 3] = channels == 2 || channels == 4 ? alpha_of(in[from + step - 1]) : 1.0F;
            }
        }

        // Through linear floats a chunk at a time, so a row of any width stays on the stack.
        template <typename Sample, typename Each>
        void chunked(const std::span<const Sample> in, const int channels, const std::span<std::uint8_t> out, const Each &each) {
            std::array<float, CHUNK * 4> held{};
            const auto step = static_cast<std::size_t>(channels);
            const std::size_t pixels = std::min(in.size() / step, out.size() / 4);

            for (std::size_t at = 0; at < pixels; at += CHUNK) {
                const std::size_t count = std::min(CHUNK, pixels - at);

                each(at, in.subspan(at * step, count * step), std::span(held).first(count * 4), out.subspan(at * 4, count * 4));
            }
        }

        // NaN and infinity turn up in float files, and would index past the table.
        float finite(const float value) {
            return value > 0.0F ? std::min(value, 1.0e6F) : 0.0F;
        }

        // Where a finite, non negative value lands in the sRGB table. Through a 32 bit int,
        // which AVX2 converts eight at a time where a size_t takes one instruction each.
        std::uint16_t slot(const float value) {
            constexpr auto TOP = static_cast<float>(ENCODED - 1);

            // NOLINTNEXTLINE(bugprone-incorrect-roundings): the value is never negative, where lround would cost a call per channel.
            return static_cast<std::uint16_t>(static_cast<std::int32_t>((std::min(value, 1.0F) * TOP) + 0.5F));
        }

        // The roll off scales all three channels by what it does to the largest, so hues hold. The
        // arithmetic runs apart from the table lookups, so it vectorises.
#if defined(__x86_64__) || defined(_M_X64)
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics): the kernel walks the rows with intrinsics.
        // NaN compares false, so it goes to zero with the negatives.
        TIV_AVX2 __m256 finite_avx2(const __m256 value) {
            return _mm256_and_ps(_mm256_cmp_ps(value, _mm256_setzero_ps(), _CMP_GT_OQ), _mm256_min_ps(value, _mm256_set1_ps(1.0e6F)));
        }

        TIV_AVX2 __m256i slots_avx2(const __m256 value, const bool pq) {
            constexpr auto TOP = static_cast<float>(ENCODED - 1);
            const __m256 one = _mm256_set1_ps(1.0F);
            const __m256 clamped = pq ? _mm256_sqrt_ps(_mm256_min_ps(_mm256_mul_ps(value, _mm256_set1_ps(1.0F / PQ_TOP)), one)) : _mm256_min_ps(value, one);

            return _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(clamped, _mm256_set1_ps(TOP)), _mm256_set1_ps(0.5F)));
        }

        TIV_AVX2 __m256i look_up_avx2(const int *table, const __m256i slot, const bool pq) {
            return pq ? _mm256_and_si256(_mm256_i32gather_epi32(table, slot, 2), _mm256_set1_epi32(0xFFFF))
                      : _mm256_and_si256(_mm256_i32gather_epi32(table, slot, 1), _mm256_set1_epi32(0xFF));
        }

        // Both encoders eight pixels at a time, the same arithmetic as the scalar loops, which
        // take whatever is left over. Returns how many pixels it did.
        TIV_AVX2 std::size_t encode_avx2(const float *rgba, std::uint8_t *out, const std::size_t pixels, const bool pq, const float headroom, const bool rolledOff) {
            const __m256 knee = _mm256_set1_ps(KNEE * headroom);
            const __m256 room = _mm256_set1_ps(headroom - (KNEE * headroom));
            const __m256 one = _mm256_set1_ps(1.0F);
            const __m256 half = _mm256_set1_ps(0.5F);
            const __m256 alphaTop = _mm256_set1_ps(pq ? 3.0F : 255.0F);
            // The transpose leaves the even pixels in the low lane and the odd ones in the high.
            const __m256i order = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
            const int *table = pq ? reinterpret_cast<const int *>(pq_table().data()) : reinterpret_cast<const int *>(srgb_table().data());
            std::size_t done = 0;

            for (; done + 8 <= pixels; done += 8) {
                const float *in = rgba + (done * 4);
                const __m256 p0 = _mm256_loadu_ps(in);
                const __m256 p1 = _mm256_loadu_ps(in + 8);
                const __m256 p2 = _mm256_loadu_ps(in + 16);
                const __m256 p3 = _mm256_loadu_ps(in + 24);
                const __m256 t0 = _mm256_unpacklo_ps(p0, p1);
                const __m256 t1 = _mm256_unpackhi_ps(p0, p1);
                const __m256 t2 = _mm256_unpacklo_ps(p2, p3);
                const __m256 t3 = _mm256_unpackhi_ps(p2, p3);
                __m256 r = finite_avx2(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0)));
                __m256 g = finite_avx2(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2)));
                __m256 b = finite_avx2(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0)));
                const __m256 a = _mm256_min_ps(finite_avx2(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2))), one);

                if (rolledOff) {
                    const __m256 peak = _mm256_max_ps(r, _mm256_max_ps(g, b));
                    const __m256 over = _mm256_max_ps(_mm256_sub_ps(peak, knee), _mm256_setzero_ps());
                    const __m256 bent = _mm256_div_ps(_mm256_add_ps(knee, _mm256_div_ps(_mm256_mul_ps(room, over), _mm256_add_ps(over, room))), peak);
                    const __m256 scale = _mm256_blendv_ps(one, bent, _mm256_cmp_ps(peak, knee, _CMP_GT_OQ));

                    r = _mm256_mul_ps(r, scale);
                    g = _mm256_mul_ps(g, scale);
                    b = _mm256_mul_ps(b, scale);
                }

                const __m256i alpha = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(a, alphaTop), half));
                const int shift = pq ? 10 : 8;
                __m256i word = look_up_avx2(table, slots_avx2(r, pq), pq);

                word = _mm256_or_si256(word, _mm256_slli_epi32(look_up_avx2(table, slots_avx2(g, pq), pq), shift));
                word = _mm256_or_si256(word, _mm256_slli_epi32(look_up_avx2(table, slots_avx2(b, pq), pq), shift * 2));
                word = _mm256_or_si256(word, _mm256_slli_epi32(alpha, pq ? 30 : 24));

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + (done * 4)), _mm256_permutevar8x32_epi32(word, order));
            }

            return done;
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
#endif

        // How far the vector kernel got, for the scalar loop to go on from.
        std::size_t encode_vector(const std::span<const float> rgba, const std::span<std::uint8_t> out, const bool pq, const float headroom, const bool rolledOff) {
#if defined(__x86_64__) || defined(_M_X64)
            if (Simd::avx2()) {
                return encode_avx2(rgba.data(), out.data(), std::min(rgba.size(), out.size()) / 4, pq, headroom, rolledOff);
            }
#else
            (void) rgba;
            (void) out;
            (void) pq;
            (void) headroom;
            (void) rolledOff;
#endif

            return 0;
        }

        void encode_srgb(const std::span<const float> rgba, const std::span<std::uint8_t> out, const bool rolledOff) {
            constexpr float ROOM = 1.0F - KNEE;
            constexpr std::size_t BLOCK = 64;
            const SrgbTable &srgb = srgb_table();
            const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;
            std::array<std::uint16_t, BLOCK * 4> slots{};

            for (std::size_t first = encode_vector(rgba, out, false, 1.0F, rolledOff); first < pixels; first += BLOCK) {
                const std::size_t count = std::min(BLOCK, pixels - first);
                const std::span<const float> in = rgba.subspan(first * 4, count * 4);

                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,bugprone-incorrect-roundings): the loops stay within the block, slot() within the table, and nothing is negative.
                for (std::size_t at = 0; at < count * 4; at += 4) {
                    const float r = finite(in[at]);
                    const float g = finite(in[at + 1]);
                    const float b = finite(in[at + 2]);
                    const float peak = std::max({r, g, b});
                    const float over = std::max(peak - KNEE, 0.0F);
                    const float scale = rolledOff && peak > KNEE ? (KNEE + (ROOM * over / (over + ROOM))) / peak : 1.0F;

                    slots[at] = slot(r * scale);
                    slots[at + 1] = slot(g * scale);
                    slots[at + 2] = slot(b * scale);
                    slots[at + 3] = static_cast<std::uint16_t>(static_cast<std::int32_t>((std::min(finite(in[at + 3]), 1.0F) * 255.0F) + 0.5F));
                }

                const std::span<std::uint8_t> bytes = out.subspan(first * 4, count * 4);

                for (std::size_t at = 0; at < count * 4; at += 4) {
                    bytes[at] = srgb[slots[at]];
                    bytes[at + 1] = srgb[slots[at + 1]];
                    bytes[at + 2] = srgb[slots[at + 2]];
                    bytes[at + 3] = static_cast<std::uint8_t>(slots[at + 3]);
                }
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,bugprone-incorrect-roundings)
            }
        }

        // By the same roll off, up to the headroom, into the words Bitmap::Encoding::Pq describes.
        void encode_pq(const std::span<const float> rgba, const std::span<std::uint8_t> out, const float headroom, const bool rolledOff) {
            constexpr std::size_t BLOCK = 64;
            constexpr auto TOP = static_cast<float>(ENCODED - 1);
            const float knee = KNEE * headroom;
            const float room = headroom - knee;
            const PqTable &pq = pq_table();
            const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;
            std::array<std::uint16_t, BLOCK * 4> slots{};

            for (std::size_t first = encode_vector(rgba, out, true, headroom, rolledOff); first < pixels; first += BLOCK) {
                const std::size_t count = std::min(BLOCK, pixels - first);
                const std::span<const float> in = rgba.subspan(first * 4, count * 4);

                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,bugprone-incorrect-roundings): the loops stay within the block, the slots within the table, and nothing is negative.
                for (std::size_t at = 0; at < count * 4; at += 4) {
                    const float r = finite(in[at]);
                    const float g = finite(in[at + 1]);
                    const float b = finite(in[at + 2]);
                    const float peak = std::max({r, g, b});
                    const float over = std::max(peak - knee, 0.0F);
                    const float scale = rolledOff && peak > knee ? (knee + (room * over / (over + room))) / peak : 1.0F;

                    slots[at] = static_cast<std::uint16_t>(static_cast<std::int32_t>((std::sqrt(std::min(r * scale * (1.0F / PQ_TOP), 1.0F)) * TOP) + 0.5F));
                    slots[at + 1] = static_cast<std::uint16_t>(static_cast<std::int32_t>((std::sqrt(std::min(g * scale * (1.0F / PQ_TOP), 1.0F)) * TOP) + 0.5F));
                    slots[at + 2] = static_cast<std::uint16_t>(static_cast<std::int32_t>((std::sqrt(std::min(b * scale * (1.0F / PQ_TOP), 1.0F)) * TOP) + 0.5F));
                    slots[at + 3] = static_cast<std::uint16_t>(static_cast<std::int32_t>((std::min(finite(in[at + 3]), 1.0F) * 3.0F) + 0.5F));
                }

                const std::span<std::uint8_t> bytes = out.subspan(first * 4, count * 4);

                for (std::size_t at = 0; at < count * 4; at += 4) {
                    const std::uint32_t word = pq[slots[at]] | (static_cast<std::uint32_t>(pq[slots[at + 1]]) << 10) | (static_cast<std::uint32_t>(pq[slots[at + 2]]) << 20)
                                               | (static_cast<std::uint32_t>(slots[at + 3]) << 30);

                    std::memcpy(bytes.subspan(at, 4).data(), &word, 4);
                }
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,bugprone-incorrect-roundings)
            }
        }
    }

    Tone::Source Tone::from_cicp(const int primaries, const int transfer) {
        Source held;

        switch (transfer) {
            case 8:
                held.transfer = Transfer::Linear;
                break;
            case 16:
                held.transfer = Transfer::Pq;
                break;
            case 18:
                held.transfer = Transfer::Hlg;
                break;
            default:
                break;
        }

        switch (primaries) {
            case 9:
                held.primaries = Primaries::Bt2020;
                break;
            case 11:
            case 12:
                held.primaries = Primaries::P3;
                break;
            default:
                break;
        }

        return held;
    }

    Tone::Matrix Tone::convert(const Primaries from, const Primaries to) {
        return multiply(invert(to_xyz(to)), to_xyz(from));
    }

    void Tone::transform(const Matrix &matrix, const std::span<float> rgba) {
        apply_matrix(matrix, rgba);
    }

    Tone::Mapper::Mapper(const Source source, const Display display, const bool rolledOff)
        : _source(source), _display(display), _table(table_for(source.transfer).data()),
          _toOutput(convert(source.primaries, display.hdr() ? Primaries::Bt2020 : Primaries::Bt709)),
          _convert(source.primaries != (display.hdr() ? Primaries::Bt2020 : Primaries::Bt709)), _rolledOff(rolledOff) {
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): an overload of the others, which read the source.
    void Tone::Mapper::linear(const std::span<const std::uint8_t> in, const int channels, const std::span<float> out) const {
        const std::array<float, 256> &table = srgb_linear();

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): the table holds every 8 bit value.
        spread(in, channels, out, [&table](const std::uint8_t sample) { return table[sample]; });
    }

    void Tone::Mapper::linear(const std::span<const std::uint16_t> in, const int channels, const std::span<float> out) const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the table holds every 16 bit value.
        spread(in, channels, out, [this](const std::uint16_t sample) { return _table[sample]; });

        if (_source.transfer == Transfer::Hlg) {
            hlg_display(_source.primaries, out);
        }
    }

    void Tone::Mapper::linear(const std::span<const float> in, const int channels, const std::span<float> out) const {
        spread(in, channels, out, [this](const float sample) { return decode(_source.transfer, sample); });

        if (_source.transfer == Transfer::Hlg) {
            hlg_display(_source.primaries, out);
        }
    }

    void Tone::Mapper::finish(const std::span<float> rgba, const std::span<std::uint8_t> out) const {
        if (_convert) {
            apply_matrix(_toOutput, rgba);
        }

        if (_display.hdr()) {
            encode_pq(rgba, out, _display.headroom, _rolledOff);
        } else {
            encode_srgb(rgba, out, _rolledOff);
        }
    }

    void Tone::Mapper::map(const std::span<const std::uint8_t> in, const int channels, const std::span<std::uint8_t> out, const Adjust &adjust) const {
        chunked(in, channels, out, [&](const std::size_t first, const auto part, const std::span<float> rgba, const std::span<std::uint8_t> bytes) {
            linear(part, channels, rgba);

            if (adjust) {
                adjust(first, rgba);
            }

            finish(rgba, bytes);
        });
    }

    void Tone::Mapper::map(const std::span<const std::uint16_t> in, const int channels, const std::span<std::uint8_t> out, const Adjust &adjust) const {
        chunked(in, channels, out, [&](const std::size_t first, const auto part, const std::span<float> rgba, const std::span<std::uint8_t> bytes) {
            linear(part, channels, rgba);

            if (adjust) {
                adjust(first, rgba);
            }

            finish(rgba, bytes);
        });
    }

    void Tone::Mapper::map(const std::span<const float> in, const int channels, const std::span<std::uint8_t> out, const Adjust &adjust) const {
        chunked(in, channels, out, [&](const std::size_t first, const auto part, const std::span<float> rgba, const std::span<std::uint8_t> bytes) {
            linear(part, channels, rgba);

            if (adjust) {
                adjust(first, rgba);
            }

            finish(rgba, bytes);
        });
    }
}
