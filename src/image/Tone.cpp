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
#include <span>
#include <type_traits>
#include <vector>

#include "image/Tone.h"

namespace tiv {
    namespace {
        // Where HDR puts SDR white: https://www.itu.int/pub/R-REP-BT.2408
        constexpr float SDR_WHITE_NITS = 203.0F;

        // HLG is shown as on the reference display of BT.2100, 1000 nits with a system gamma of 1.2.
        constexpr float HLG_PEAK_NITS = 1000.0F;
        constexpr float HLG_GAMMA = 1.2F;

        // Linear light up to here is left alone, the rest bends towards 1 without reaching it.
        constexpr float KNEE = 0.8F;

        constexpr std::size_t SAMPLES = 65536;
        constexpr std::size_t ENCODED = 16384;
        constexpr std::size_t CHUNK = 256;

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

        // The sRGB curve of IEC 61966-2-1 over linear light from 0 to 1.
        const std::array<std::uint8_t, ENCODED> &srgb_table() {
            static const std::array<std::uint8_t, ENCODED> TABLE = [] {
                std::array<std::uint8_t, ENCODED> held{};

                for (std::size_t i = 0; i < ENCODED; ++i) {
                    const double v = static_cast<double>(i) / static_cast<double>(ENCODED - 1);
                    const double encoded = v <= 0.0031308 ? v * 12.92 : (1.055 * std::pow(v, 1.0 / 2.4)) - 0.055;

                    held.at(i) = static_cast<std::uint8_t>(std::lround(encoded * 255.0));
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

    Tone::Mapper::Mapper(const Source source, const bool rolledOff)
        : _source(source), _table(table_for(source.transfer).data()), _toBt709(convert(source.primaries, Primaries::Bt709)), _convert(source.primaries != Primaries::Bt709),
          _rolledOff(rolledOff) {
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
            apply_matrix(_toBt709, rgba);
        }

        encode(rgba, out, _rolledOff);
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

    // The roll off scales all three channels by what it does to the largest, so hues hold. The
    // arithmetic runs apart from the table lookups, so it vectorises.
    void Tone::encode(const std::span<const float> rgba, const std::span<std::uint8_t> out, const bool rolledOff) {
        constexpr float ROOM = 1.0F - KNEE;
        constexpr std::size_t BLOCK = 64;
        const std::array<std::uint8_t, ENCODED> &srgb = srgb_table();
        const std::size_t pixels = std::min(rgba.size(), out.size()) / 4;
        std::array<std::uint16_t, BLOCK * 4> slots{};

        for (std::size_t first = 0; first < pixels; first += BLOCK) {
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
}
