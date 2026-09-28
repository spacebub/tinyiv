// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "image/decode/Support.h"

namespace tiv {
    void Decode::fail(std::string *error, const std::filesystem::path &file, const std::string_view why) {
        if (error != nullptr) {
            *error = file.string() + ": " + std::string(why);
        }
    }

    Decode::Size Decode::fitted(const int width, const int height, const int boxWidth, const int boxHeight) {
        if (width <= boxWidth && height <= boxHeight) {
            return {width, height};
        }

        const double shrink = std::min(static_cast<double>(boxWidth) / width, static_cast<double>(boxHeight) / height);

        return {std::max(static_cast<int>(std::floor(width * shrink)), 1), std::max(static_cast<int>(std::floor(height * shrink)), 1)};
    }

    bool Decode::starts_with(const std::span<const std::uint8_t> head, const std::string_view magic, const std::size_t at) {
        if (head.size() < at + magic.size()) {
            return false;
        }

        return std::equal(magic.begin(), magic.end(), head.begin() + static_cast<std::ptrdiff_t>(at), [](const char a, const std::uint8_t b) {
            return static_cast<std::uint8_t>(a) == b;
        });
    }

    int Decode::shrink_factor(const int width, const int height, const int boxWidth, const int boxHeight) {
        int factor = 1;

        while ((width + factor - 1) / factor > boxWidth || (height + factor - 1) / factor > boxHeight) {
            ++factor;
        }

        return factor;
    }
}
