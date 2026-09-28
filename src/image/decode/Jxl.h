// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_JXL_H
#define TIV_DECODE_JXL_H


#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"

namespace tiv::Decode::Jxl {

    [[nodiscard]] Tone::Source tone(std::span<const std::uint8_t> data);

    bool probe(std::span<const std::uint8_t> data, Info *info);

    // Animation stays with libvips.
    Direct load(const std::filesystem::path &file, std::span<const std::uint8_t> data, Bitmap *out, std::string *error,
                const Abort *abort, const Tone::Display &display);

    // Band by band into take, in however many threads libjxl has, with only a few bands
    // held. Animation stays with libvips.
    Direct stream(std::span<const std::uint8_t> data, int rows, const Begin &begin, const Take &take,
                  const Abort *abort, const Tone::Display &display);

    // About the most memory stream() holds in bands of the rows given.
    [[nodiscard]] std::uint64_t stream_bytes(std::span<const std::uint8_t> data, int rows);

}


#endif //TIV_DECODE_JXL_H
