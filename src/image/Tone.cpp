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

        using Luma = std::array<float, 3>;

        // The Y row of the primaries' RGB to XYZ.
        Luma luma_of(const Tone::Primaries primaries) {
            const Matrix xyz = to_xyz(primaries);

            return {xyz.at(3), xyz.at(4), xyz.at(5)};
        }

#if defined(__x86_64__) || defined(_M_X64)
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics): the kernels walk the rows with intrinsics.
        // For positive x, by polynomials fitted to log2 over [1, 2) and exp2 over [0, 1), within 1e-5.
        TIV_AVX2 __m256 log2_avx2(const __m256 x) {
            const __m256i bits = _mm256_castps_si256(x);
            const __m256 exponent = _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(127)));
            const __m256 t = _mm256_sub_ps(_mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(bits, _mm256_set1_epi32(0x7FFFFF)), _mm256_set1_epi32(0x3F800000))), _mm256_set1_ps(1.0F));
            __m256 p = _mm256_set1_ps(0.04392957F);

            p = _mm256_add_ps(_mm256_mul_ps(p, t), _mm256_set1_ps(-0.18983641F));
            p = _mm256_add_ps(_mm256_mul_ps(p, t), _mm256_set1_ps(0.41156681F));
            p = _mm256_add_ps(_mm256_mul_ps(p, t), _mm256_set1_ps(-0.70725636F));
            p = _mm256_add_ps(_mm256_mul_ps(p, t), _mm256_set1_ps(1.44159271F));
            p = _mm256_add_ps(_mm256_mul_ps(p, t), _mm256_set1_ps(0.00001435F));

            return _mm256_add_ps(exponent, p);
        }

        TIV_AVX2 __m256 exp2_avx2(const __m256 x) {
            const __m256 clamped = _mm256_max_ps(x, _mm256_set1_ps(-126.0F));
            const __m256 whole = _mm256_floor_ps(clamped);
            const __m256 f = _mm256_sub_ps(clamped, whole);
            __m256 p = _mm256_set1_ps(0.01368400F);

            p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(0.05171783F));
            p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(0.24162116F));
            // NOLINTNEXTLINE(modernize-use-std-numbers): a fitted coefficient, near ln 2 but not it.
            p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(0.69296962F));
            p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(1.00000359F));

            const __m256i scale = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(whole), _mm256_set1_epi32(127)), 23);

            return _mm256_mul_ps(p, _mm256_castsi256_ps(scale));
        }

        // Interleaved RGBA of eight pixels into planes and back: the even pixels land in the low lane, the odd in the high.
        struct Planes {
            __m256 r;
            __m256 g;
            __m256 b;
            __m256 a;
        };

        TIV_AVX2 Planes split_avx2(const float *in) {
            const __m256 p0 = _mm256_loadu_ps(in);
            const __m256 p1 = _mm256_loadu_ps(in + 8);
            const __m256 p2 = _mm256_loadu_ps(in + 16);
            const __m256 p3 = _mm256_loadu_ps(in + 24);
            const __m256 t0 = _mm256_unpacklo_ps(p0, p1);
            const __m256 t1 = _mm256_unpackhi_ps(p0, p1);
            const __m256 t2 = _mm256_unpacklo_ps(p2, p3);
            const __m256 t3 = _mm256_unpackhi_ps(p2, p3);

            return {_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0)), _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2)), _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0)),
                    _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2))};
        }

        TIV_AVX2 void join_avx2(const Planes &planes, float *out) {
            const __m256 rg0 = _mm256_unpacklo_ps(planes.r, planes.g);
            const __m256 rg1 = _mm256_unpackhi_ps(planes.r, planes.g);
            const __m256 ba0 = _mm256_unpacklo_ps(planes.b, planes.a);
            const __m256 ba1 = _mm256_unpackhi_ps(planes.b, planes.a);

            _mm256_storeu_ps(out, _mm256_shuffle_ps(rg0, ba0, _MM_SHUFFLE(1, 0, 1, 0)));
            _mm256_storeu_ps(out + 8, _mm256_shuffle_ps(rg0, ba0, _MM_SHUFFLE(3, 2, 3, 2)));
            _mm256_storeu_ps(out + 16, _mm256_shuffle_ps(rg1, ba1, _MM_SHUFFLE(1, 0, 1, 0)));
            _mm256_storeu_ps(out + 24, _mm256_shuffle_ps(rg1, ba1, _MM_SHUFFLE(3, 2, 3, 2)));
        }

        TIV_AVX2 std::size_t hlg_avx2(const Luma &luma, float *rgba, const std::size_t pixels, const float scale) {
            const auto [lr, lg, lb] = luma;
            const __m256 kr = _mm256_set1_ps(lr);
            const __m256 kg = _mm256_set1_ps(lg);
            const __m256 kb = _mm256_set1_ps(lb);
            const __m256 zero = _mm256_setzero_ps();
            std::size_t done = 0;

            for (; done + 8 <= pixels; done += 8) {
                float *at = rgba + (done * 4);
                Planes planes = split_avx2(at);
                const __m256 luminance = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(kr, planes.r), _mm256_mul_ps(kg, planes.g)), _mm256_mul_ps(kb, planes.b));
                const __m256 power = exp2_avx2(_mm256_mul_ps(log2_avx2(luminance), _mm256_set1_ps(HLG_GAMMA - 1.0F)));
                const __m256 gain = _mm256_and_ps(_mm256_cmp_ps(luminance, zero, _CMP_GT_OQ), _mm256_mul_ps(power, _mm256_set1_ps(scale)));

                planes.r = _mm256_mul_ps(planes.r, gain);
                planes.g = _mm256_mul_ps(planes.g, gain);
                planes.b = _mm256_mul_ps(planes.b, gain);
                join_avx2(planes, at);
            }

            return done;
        }

        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
#endif

        // HLG's OOTF, which brightens by the luminance of the scene: BT.2100, table 5.
        void hlg_display(const Luma &luma, std::span<float> rgba) {
            const auto [kr, kg, kb] = luma;
            const float scale = HLG_PEAK_NITS / SDR_WHITE_NITS;
            std::size_t first = 0;

#if defined(__x86_64__) || defined(_M_X64)
            if (Simd::avx2()) {
                first = hlg_avx2(luma, rgba.data(), rgba.size() / 4, scale) * 4;
            }
#endif

            for (std::size_t at = first; at + 4 <= rgba.size(); at += 4) {
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
        // take whatever is left over. The matrix, if any, goes first. Returns how many pixels it did.
        template <bool Pq, bool Convert>
        TIV_AVX2 std::size_t encode_avx2(const float *rgba, std::uint8_t *out, const std::size_t pixels, const float headroom, const bool rolledOff, const Matrix &matrix) {
            const __m256 knee = _mm256_set1_ps(KNEE * headroom);
            const __m256 room = _mm256_set1_ps(headroom - (KNEE * headroom));
            const __m256 one = _mm256_set1_ps(1.0F);
            const __m256 half = _mm256_set1_ps(0.5F);
            const __m256 alphaTop = _mm256_set1_ps(Pq ? 3.0F : 255.0F);
            // The transpose leaves the even pixels in the low lane and the odd ones in the high.
            const __m256i order = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
            const int *table = Pq ? reinterpret_cast<const int *>(pq_table().data()) : reinterpret_cast<const int *>(srgb_table().data());
            const auto [c0, c1, c2, c3, c4, c5, c6, c7, c8] = matrix;
            const __m256 m0 = _mm256_set1_ps(c0);
            const __m256 m1 = _mm256_set1_ps(c1);
            const __m256 m2 = _mm256_set1_ps(c2);
            const __m256 m3 = _mm256_set1_ps(c3);
            const __m256 m4 = _mm256_set1_ps(c4);
            const __m256 m5 = _mm256_set1_ps(c5);
            const __m256 m6 = _mm256_set1_ps(c6);
            const __m256 m7 = _mm256_set1_ps(c7);
            const __m256 m8 = _mm256_set1_ps(c8);
            std::size_t done = 0;

            for (; done + 8 <= pixels; done += 8) {
                const Planes in = split_avx2(rgba + (done * 4));
                __m256 r = in.r;
                __m256 g = in.g;
                __m256 b = in.b;

                if constexpr (Convert) {
                    r = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m0, in.r), _mm256_mul_ps(m1, in.g)), _mm256_mul_ps(m2, in.b));
                    g = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m3, in.r), _mm256_mul_ps(m4, in.g)), _mm256_mul_ps(m5, in.b));
                    b = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m6, in.r), _mm256_mul_ps(m7, in.g)), _mm256_mul_ps(m8, in.b));
                }

                r = finite_avx2(r);
                g = finite_avx2(g);
                b = finite_avx2(b);
                const __m256 a = _mm256_min_ps(finite_avx2(in.a), one);

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
                const int shift = Pq ? 10 : 8;
                __m256i word = look_up_avx2(table, slots_avx2(r, Pq), Pq);

                word = _mm256_or_si256(word, _mm256_slli_epi32(look_up_avx2(table, slots_avx2(g, Pq), Pq), shift));
                word = _mm256_or_si256(word, _mm256_slli_epi32(look_up_avx2(table, slots_avx2(b, Pq), Pq), shift * 2));
                word = _mm256_or_si256(word, _mm256_slli_epi32(alpha, Pq ? 30 : 24));

                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + (done * 4)), _mm256_permutevar8x32_epi32(word, order));
            }

            return done;
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast,portability-simd-intrinsics)
#endif

        // How far the vector kernel got, for the scalar loop to go on from.
        std::size_t encode_vector(const std::span<const float> rgba, const std::span<std::uint8_t> out, const bool pq, const float headroom, const bool rolledOff, const Matrix *matrix) {
#if defined(__x86_64__) || defined(_M_X64)
            if (Simd::avx2()) {
                const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;

                if (pq) {
                    return matrix != nullptr ? encode_avx2<true, true>(rgba.data(), out.data(), pixels, headroom, rolledOff, *matrix)
                                             : encode_avx2<true, false>(rgba.data(), out.data(), pixels, headroom, rolledOff, Matrix{});
                }

                return matrix != nullptr ? encode_avx2<false, true>(rgba.data(), out.data(), pixels, headroom, rolledOff, *matrix)
                                         : encode_avx2<false, false>(rgba.data(), out.data(), pixels, headroom, rolledOff, Matrix{});
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

        void encode_srgb(const std::span<const float> rgba, const std::span<std::uint8_t> out, const bool rolledOff, const std::size_t from) {
            constexpr float ROOM = 1.0F - KNEE;
            constexpr std::size_t BLOCK = 64;
            const SrgbTable &srgb = srgb_table();
            const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;
            std::array<std::uint16_t, BLOCK * 4> slots{};

            for (std::size_t first = from; first < pixels; first += BLOCK) {
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
        void encode_pq(const std::span<const float> rgba, const std::span<std::uint8_t> out, const float headroom, const bool rolledOff, const std::size_t from) {
            constexpr std::size_t BLOCK = 64;
            constexpr auto TOP = static_cast<float>(ENCODED - 1);
            const float knee = KNEE * headroom;
            const float room = headroom - knee;
            const PqTable &pq = pq_table();
            const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;
            std::array<std::uint16_t, BLOCK * 4> slots{};

            for (std::size_t first = from; first < pixels; first += BLOCK) {
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
          _luma(luma_of(source.primaries)), _convert(source.primaries != (display.hdr() ? Primaries::Bt2020 : Primaries::Bt709)), _rolledOff(rolledOff) {
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
            hlg_display(_luma, out);
        }
    }

    void Tone::Mapper::linear(const std::span<const float> in, const int channels, const std::span<float> out) const {
        if (_source.transfer == Transfer::Linear || _source.transfer == Transfer::Sdr) {
            spread(in, channels, out, [](const float sample) { return sample; });
        } else {
            spread(in, channels, out, [this](const float sample) { return decode(_source.transfer, sample); });
        }

        if (_source.transfer == Transfer::Hlg) {
            hlg_display(_luma, out);
        }
    }

    void Tone::Mapper::finish(const std::span<float> rgba, const std::span<std::uint8_t> out) const {
        const bool pq = _display.hdr();
        const float headroom = pq ? _display.headroom : 1.0F;
        const std::size_t done = encode_vector(rgba, out, pq, headroom, _rolledOff, _convert ? &_toOutput : nullptr);

        if (_convert) {
            apply_matrix(_toOutput, rgba.subspan(std::min(done * 4, rgba.size())));
        }

        if (pq) {
            encode_pq(rgba, out, headroom, _rolledOff, done);
        } else {
            encode_srgb(rgba, out, _rolledOff, done);
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
