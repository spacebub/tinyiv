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
    // The user's settings, one key = value a line, # starting a comment.
    struct Config {
        // Where tile caches go. Empty keeps each beside its image, and a relative path starts from
        // the image's folder.
        std::filesystem::path cache;

        // A huge PNG keeps only the points decoding can resume from, about a third of the space, instead
        // of every tile, at the cost of waiting for bands of rows when panning at full size.
        bool small = false;

        // Streaming mode is on from the start, before S is ever pressed.
        bool streaming = false;

        // $XDG_CONFIG_HOME/tinyiv/tinyiv.conf, or tinyiv.conf beside the executable on Windows.
        [[nodiscard]] static std::filesystem::path location();

        // Reads the file, then adds every setting it lacks with its default, making the file first
        // when there is none. Lines it cannot take are skipped and described in warnings.
        [[nodiscard]] static Config load(const std::filesystem::path &file, std::vector<std::string> *warnings);
    };
}


#endif //TIV_APP_CONFIG_H
