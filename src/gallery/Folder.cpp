// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gallery/Folder.h"

namespace tiv {
    namespace {
        constexpr std::string_view DIGITS = "0123456789";

        std::string lowercase(std::string text) {
            for (char &c : text) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return text;
        }

        bool starts_with_digit(const std::string_view text) {
            return !text.empty() && DIGITS.contains(text.front());
        }

        // Splits off the leading digit run without its leading zeros.
        std::string_view take_number(std::string_view *text) {
            const std::size_t start = std::min(text->find_first_not_of('0'), text->size());
            const std::size_t end = std::min(text->find_first_not_of(DIGITS, start), text->size());
            const std::string_view number = text->substr(start, end - start);

            text->remove_prefix(end);

            return number;
        }

        // Shorter numbers are smaller, equal lengths compare as text.
        int compare_numbers(const std::string_view a, const std::string_view b) {
            if (a.size() != b.size()) {
                return a.size() < b.size() ? -1 : 1;
            }

            return a.compare(b);
        }
    }

    bool Folder::open(const std::filesystem::path &file, const std::span<const std::string_view> suffixes, std::string *error) {
        std::error_code failure;
        const std::filesystem::path opened = std::filesystem::absolute(file, failure);

        if (failure || !std::filesystem::is_regular_file(opened, failure)) {
            if (error != nullptr) {
                *error = "not a file: " + file.string();
            }

            return false;
        }

        std::vector<std::filesystem::path> found;

        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(opened.parent_path(), failure)) {
            if (entry.path() == opened || !entry.is_regular_file(failure)) {
                continue;
            }

            const std::string suffix = lowercase(entry.path().extension().string());

            if (std::ranges::find(suffixes, suffix) != suffixes.end()) {
                found.push_back(entry.path());
            }
        }

        found.push_back(opened);

        std::ranges::sort(found, [](const std::filesystem::path &a, const std::filesystem::path &b) {
            return natural_less(a.filename().string(), b.filename().string());
        });

        _files = std::move(found);
        _index = static_cast<int>(std::ranges::find(_files, opened) - _files.begin());

        return true;
    }

    const std::filesystem::path &Folder::at(const int index) const {
        return _files.at(static_cast<std::size_t>(wrap(index)));
    }

    int Folder::wrap(const int index) const {
        const int total = count();
        const int rest = index % total;

        return rest < 0 ? rest + total : rest;
    }

    void Folder::step(const int delta) {
        _index = wrap(_index + delta);
    }

    bool Folder::natural_less(const std::string &a, const std::string &b) {
        std::string_view restA = a;
        std::string_view restB = b;

        while (!restA.empty() && !restB.empty()) {
            if (starts_with_digit(restA) && starts_with_digit(restB)) {
                const int order = compare_numbers(take_number(&restA), take_number(&restB));

                if (order != 0) {
                    return order < 0;
                }

                continue;
            }

            const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(restA.front())));
            const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(restB.front())));

            if (ca != cb) {
                return ca < cb;
            }

            restA.remove_prefix(1);
            restB.remove_prefix(1);
        }

        return restA.size() < restB.size();
    }
}
