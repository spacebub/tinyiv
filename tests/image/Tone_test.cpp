// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>

#include "Check.h"

#include "image/Tone.h"

namespace test {
namespace {
// Red of the first pixel, in PQ code values from 0 to 1023.
int red_of(const std::array<std::uint8_t, 4> &word) {
    const std::uint32_t packed = word.at(0) | (static_cast<std::uint32_t>(word.at(1)) << 8U);

    return static_cast<int>(packed & 0x3FFU);
}

int encode_white(const tiv::Tone::Display &display) {
    const tiv::Tone::Mapper mapper({.transfer = tiv::Tone::Transfer::Linear, .primaries = tiv::Tone::Primaries::Bt2020},
                                   display);
    const std::array<float, 4> white{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<std::uint8_t, 4> out{};

    mapper.map(std::span<const float>(white), 4, out);

    return red_of(out);
}

// SDR white lands on the nits the display asks for, 203 included.
void white_moves() {
    for (const float nits : {203.0F, 100.0F, 300.0F}) {
        const int got = encode_white({.headroom = 4.0F, .whiteNits = nits});
        const auto want = static_cast<int>(std::lround(tiv::Tone::pq_signal(nits) * 1023.0F));

        check(std::abs(got - want) <= 1, std::format("white at {} nits: code {}, want {}", nits, got, want));
    }
}

void pq_signal_points() {
    for (const float nits : {0.1F, 1.0F, 80.0F, 203.0F, 1000.0F, 10000.0F}) {
        const float signal = tiv::Tone::pq_signal(nits);

        check(signal >= 0.0F && signal <= 1.0F, std::format("signal for {} nits is {}", nits, signal));
    }

    check(std::abs(tiv::Tone::pq_signal(10000.0F) - 1.0F) < 1e-5F, "10000 nits is the top of PQ");
    check(std::abs(tiv::Tone::pq_signal(203.0F) - 0.5807F) < 1e-3F, "203 nits is PQ 0.58");
    check(tiv::Tone::from_srgb(255) == 1.0F && tiv::Tone::from_srgb(0) == 0.0F, "sRGB ends map to 0 and 1");
}
}

void register_tone() {
    add("tone/white_moves", white_moves);
    add("tone/pq_signal_points", pq_signal_points);
}
}
