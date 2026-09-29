// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_CONFIG_H
#define TIV_APP_CONFIG_H


#include <filesystem>
#include <string>
#include <vector>

namespace tiv {
    // The user's settings, one `key = value` a line, with `#` starting a comment.
    struct Config {
        // Where pyramids on disk go. Empty puts them beside each image, and a relative path
        // is taken from the image's folder.
        std::filesystem::path cache;

        // $XDG_CONFIG_HOME/tinyiv/tinyiv.conf, or tinyiv.conf beside the executable on Windows.
        [[nodiscard]] static std::filesystem::path location();

        // Reads the file, and writes one listing every key when there is none, so it can be
        // found and edited. Lines it cannot take are described in warnings and skipped.
        [[nodiscard]] static Config load(const std::filesystem::path &file, std::vector<std::string> *warnings);
    };
}


#endif //TIV_APP_CONFIG_H
