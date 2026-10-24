// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_HARNESS_CORPUS_H
#define TIV_HARNESS_CORPUS_H


#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "image/decode/Decode.h"

namespace harness {
    enum class Size : std::uint8_t {
        // Seconds to write and quick under the sanitizers.
        Small,
        // Large enough that the profile shows where the time goes.
        Full,
    };

    // One file of the corpus and what tinyiv has to make of it.
    struct Entry {
        // The folder and the name, relative to the corpus root.
        std::filesystem::path file;
        // As Decode::Info says it. Empty for a broken file.
        std::string format;
        tiv::Decode::Format kind = tiv::Decode::Format::Other;
        // As shown, with the orientation applied.
        int width = 0;
        int height = 0;
        int orientation = 1;
        int frames = 1;
        bool hdr = false;
        // Has to fail to open.
        bool broken = false;
    };

    // Writes the corpus under the root. A stamp skips the writing when the corpus already matches
    // this size and generator. Every file is then checked against the entry that describes it, and
    // each problem goes into failed.
    std::vector<Entry> prepare_corpus(const std::filesystem::path &root, Size size, std::vector<std::string> *failed);
}


#endif //TIV_HARNESS_CORPUS_H
