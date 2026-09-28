// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_TONE_H
#define TIV_IMAGE_TONE_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace tiv::Tone {

    enum class Transfer : std::uint8_t {
        Sdr,
        // Light as it is, SDR white at 1.
        Linear,
        Pq,
        Hlg,
    };

    enum class Primaries : std::uint8_t {
        Bt709,
        P3,
        Bt2020,
    };

    struct Source {
        Transfer transfer = Transfer::Sdr;
        Primaries primaries = Primaries::Bt709;

        [[nodiscard]] bool hdr() const { return transfer != Transfer::Sdr; }
    };

    // What the image is shown on. Headroom is how much brighter than SDR white it goes, 1 for
    // an SDR display.
    struct Display {
        float headroom = 1.0F;

        [[nodiscard]] bool hdr() const { return headroom > 1.0F; }
    };

    // From the code points of ITU-T H.273, which nclx, cICP and JPEG XL carry.
    [[nodiscard]] Source from_cicp(int primaries, int transfer);

    // Linear RGB from one set of primaries to another, row major.
    using Matrix = std::array<float, 9>;

    [[nodiscard]] Matrix convert(Primaries from, Primaries to);

    // Over linear RGBA in place.
    void transform(const Matrix &matrix, std::span<float> rgba);

    // Changes linear light before it is written, told the index of its first pixel in the row.
    using Adjust = std::function<void(std::size_t first, std::span<float> rgba)>;

    // Pixels of 1 to 4 channels: grey, grey and alpha, RGB, RGBA.
    class Mapper {

    public:
        // For an SDR display the pixels come out RGBA8 sRGB, for an HDR one PQ as Bitmap
        // lays it out. Light beyond the display's headroom rolls off into it, or without the
        // roll off clips, as it should once a gain map has brought it down.
        explicit Mapper(Source source, Display display = {}, bool rolledOff = true);

        // To RGBA in linear light of the source's primaries, SDR white at 1. Samples span
        // their full range, and 8 bit ones are SDR.
        void linear(std::span<const std::uint8_t> in, int channels, std::span<float> out) const;
        void linear(std::span<const std::uint16_t> in, int channels, std::span<float> out) const;
        void linear(std::span<const float> in, int channels, std::span<float> out) const;

        // Four bytes a pixel out, which may be where 8 bit samples came in.
        void map(std::span<const std::uint8_t> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;
        void map(std::span<const std::uint16_t> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;
        void map(std::span<const float> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;

    private:
        void finish(std::span<float> rgba, std::span<std::uint8_t> out) const;

        Source _source;
        Display _display;
        const float *_table = nullptr;
        Matrix _toOutput{};
        bool _convert = false;
        bool _rolledOff = true;
    };

}


#endif //TIV_IMAGE_TONE_H
