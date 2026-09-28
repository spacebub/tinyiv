// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_SUPPORT_H
#define TIV_DECODE_SUPPORT_H


#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "image/decode/Decode.h"

namespace tiv::Decode {

    constexpr int MAX_THREADS = 8;

    // The direct decoders check for an abort every this many rows.
    constexpr int ABORT_ROWS = 64;

    enum class Direct : std::uint8_t {
        Done,
        Failed,
        // Not this decoder's format, or a variant it leaves to libvips.
        Skip,
    };

    struct Size {
        int width = 0;
        int height = 0;
    };

    void fail(std::string *error, const std::filesystem::path &file, std::string_view why);

    [[nodiscard]] inline bool aborted(const Abort *abort) {
        return abort != nullptr && abort->requested();
    }

    // The size the image has once fitted in the box, never enlarged.
    [[nodiscard]] Size fitted(int width, int height, int boxWidth, int boxHeight);

    [[nodiscard]] bool starts_with(std::span<const std::uint8_t> head, std::string_view magic, std::size_t at = 0);

    // The smallest integer shrink that brings the size within the box.
    [[nodiscard]] int shrink_factor(int width, int height, int boxWidth, int boxHeight);

}


#endif //TIV_DECODE_SUPPORT_H
