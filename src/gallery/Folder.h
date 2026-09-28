// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_GALLERY_FOLDER_H
#define TIV_GALLERY_FOLDER_H


#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiv {
    class Folder {

    public:
        // Suffixes are lowercase with the dot. The opened file is listed whatever its suffix.
        bool open(const std::filesystem::path &file, std::span<const std::string_view> suffixes,
                  std::string *error = nullptr);

        [[nodiscard]] int count() const { return static_cast<int>(_files.size()); }
        [[nodiscard]] int index() const { return _index; }
        [[nodiscard]] const std::filesystem::path &current() const {
            return _files.at(static_cast<std::size_t>(_index));
        }
        [[nodiscard]] const std::filesystem::path &at(int index) const;

        [[nodiscard]] int wrap(int index) const;
        void step(int delta);

        // Digit runs compare by value, letters without case: img2 before img10.
        [[nodiscard]] static bool natural_less(const std::string &a, const std::string &b);

    private:
        std::vector<std::filesystem::path> _files;
        int _index = 0;
    };
}


#endif //TIV_GALLERY_FOLDER_H
