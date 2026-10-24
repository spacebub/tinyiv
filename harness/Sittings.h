// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_HARNESS_SITTINGS_H
#define TIV_HARNESS_SITTINGS_H


#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Corpus.h"
#include "Driver.h"

namespace harness {
    // Finds corpus entries for the sittings.
    class Library {

    public:
        explicit Library(std::vector<Entry> entries) : _entries(std::move(entries)) {}

        // File names are unique across the corpus, so a copy elsewhere is found by name.
        [[nodiscard]] const Entry *find(const std::filesystem::path &file) const;
        [[nodiscard]] std::vector<const Entry *> in(std::string_view folder) const;
        // Any file in the corpus folder, to open the folder with.
        [[nodiscard]] std::filesystem::path any(std::string_view folder) const;

    private:
        std::vector<Entry> _entries;
    };

    // A session of tinyiv under one set of settings, scripted as a person would use it.
    struct Sitting {
        std::string_view name;
        std::string_view about;
        void (*run)(const Library &library, Driver &driver);
    };

    [[nodiscard]] std::span<const Sitting> sittings();
}


#endif //TIV_HARNESS_SITTINGS_H
