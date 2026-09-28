// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_BENCH_SUPPORT_CORPUS_H
#define TIV_BENCH_SUPPORT_CORPUS_H


#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bench::Corpus {

    // Textured noise, so the file compresses like a detailed photograph.
    struct Spec {
        std::string_view name;
        // The libvips save options, after the suffix.
        std::string_view options;
        // What the file should weigh, or zero to take the dimensions as given.
        std::size_t targetBytes;
        int width;
        int height;
        // Noise on top of the texture. Lossy formats need more to reach the weight.
        double sigma;
    };

    // Writes the missing files first, which takes a few minutes once.
    void prepare(const std::filesystem::path &dir);

    [[nodiscard]] const std::filesystem::path &dir();
    [[nodiscard]] std::span<const Spec> specs();
    [[nodiscard]] std::filesystem::path file(std::string_view name);
    [[nodiscard]] std::vector<std::filesystem::path> files();

}


#endif //TIV_BENCH_SUPPORT_CORPUS_H
