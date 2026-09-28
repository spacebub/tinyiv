// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_WEBP_H
#define TIV_DECODE_WEBP_H


#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"

namespace tiv::Decode::WebP {

    // An animation is left to libvips, which counts its frames.
    bool probe(std::span<const std::uint8_t> data, Info *info);

    // Native size, since libwebp's own scaling measured slower than a full decode and halving.
    // Animation stays with libvips.
    Direct load(const std::filesystem::path &file, std::span<const std::uint8_t> data, Bitmap *out, std::string *error,
                const Abort *abort);

}


#endif //TIV_DECODE_WEBP_H
