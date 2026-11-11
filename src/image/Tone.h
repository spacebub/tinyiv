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

// The reference white of PQ, which BT.2408 and the colour protocol anchor to.
inline constexpr float PQ_REFERENCE_NITS = 203.0F;

// What the image is shown on. Headroom is how much brighter than SDR white it goes, 1 for
// an SDR display.
struct Display {
    float headroom = 1.0F;
    // The Vulkan renderer passes PQ through untouched, so white is set in the encoding. The SDL renderer
    // scales it when drawing.
    float whiteNits = PQ_REFERENCE_NITS;

    [[nodiscard]] bool hdr() const { return headroom > 1.0F; }
    bool operator==(const Display &) const = default;
};

// The PQ signal from 0 to 1 for light in nits, the inverse EOTF of SMPTE ST 2084.
[[nodiscard]] float pq_signal(float nits);

// An sRGB code value from 0 to 255 as linear light, SDR white at 1.
[[nodiscard]] float from_srgb(std::uint8_t value);

// From the code points of ITU-T H.273, which nclx, cICP and JPEG XL carry.
[[nodiscard]] Source from_cicp(int primaries, int transfer);

// Linear RGB from one set of primaries to another, row major.
using Matrix = std::array<float, 9>;

[[nodiscard]] Matrix convert(Primaries from, Primaries to);

// Over linear RGBA in place.
void transform(const Matrix &matrix, std::span<float> rgba);

// Changes linear light before it is written. First is the index of the first pixel in the row.
using Adjust = std::function<void(std::size_t first, std::span<float> rgba)>;

// Pixels of 1 to 4 channels: grey, grey and alpha, RGB, RGBA.
class Mapper {

public:
    // For an SDR display the pixels come out RGBA8 sRGB, and for an HDR one PQ as Bitmap lays it out.
    // Light beyond the display headroom rolls off into it. Without the roll off it clips, as it should
    // once a gain map has brought it down.
    explicit Mapper(Source source, Display display = Display(), bool rolledOff = true);

    // To RGBA in linear light of the source's primaries, SDR white at 1. Samples span
    // their full range, and 8 bit ones are SDR.
    void linear(std::span<const std::uint8_t> in, int channels, std::span<float> out) const;
    void linear(std::span<const std::uint16_t> in, int channels, std::span<float> out) const;
    void linear(std::span<const float> in, int channels, std::span<float> out) const;

    // Four bytes a pixel out, which may be where 8 bit samples came in.
    void
    map(std::span<const std::uint8_t> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;
    void
    map(std::span<const std::uint16_t> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;
    void map(std::span<const float> in, int channels, std::span<std::uint8_t> out, const Adjust &adjust = {}) const;

private:
    void finish(std::span<float> rgba, std::span<std::uint8_t> out) const;

    Source _source;
    Display _display;
    const float *_table = nullptr;
    Matrix _toOutput{};
    // The Y row of the source's primaries, for HLG.
    std::array<float, 3> _luma{};
    bool _convert = false;
    bool _rolledOff = true;
};

}


#endif //TIV_IMAGE_TONE_H
