// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_PNG_H
#define TIV_DECODE_PNG_H


#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"

namespace tiv::Decode::Png {

    // https://www.w3.org/TR/png-3/#cICP-chunk
    [[nodiscard]] Tone::Source tone(std::span<const std::uint8_t> data);

    bool probe(std::span<const std::uint8_t> data, Info *info);

    Direct load(const std::filesystem::path &file, std::span<const std::uint8_t> data, int boxWidth, int boxHeight,
                Bitmap *out, std::string *error, const Abort *abort, Fit fit, const Tone::Display &display = {});

}


#endif //TIV_DECODE_PNG_H
