// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_HARNESS_SANDBOX_H
#define TIV_HARNESS_SANDBOX_H


#include <cstddef>
#include <filesystem>
#include <string_view>

namespace harness::Sandbox {
    // HOME, XDG_CONFIG_HOME and XDG_CACHE_HOME go under the root, so runs never touch the real
    // config or caches. SDL draws offscreen unless SDL_VIDEO_DRIVER says otherwise. Each corpus
    // size gets a separate folder, so switching sizes writes nothing again.
    void enter(const std::filesystem::path &root, std::string_view corpus);

    [[nodiscard]] const std::filesystem::path &root();
    [[nodiscard]] std::filesystem::path corpus();

    // Returns an empty folder under work/. The harness empties a folder left by an earlier run,
    // but only when the folder carries the harness mark.
    [[nodiscard]] std::filesystem::path fresh(std::string_view name);

    // A copy of a folder of the corpus, for sittings that write into their files.
    void copy(const std::filesystem::path &from, const std::filesystem::path &to);

    // Where tinyiv reads its settings.
    [[nodiscard]] std::filesystem::path config();
    void write_config(std::string_view text);

    // Bytes resident at the most since the process started.
    [[nodiscard]] std::size_t peak();
}


#endif //TIV_HARNESS_SANDBOX_H
