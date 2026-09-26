// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_MAPPED_H
#define TIV_IMAGE_MAPPED_H


#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace tiv {
    // A whole file as read only memory, so a decoder reads it in place. Nothing is copied.
    class Mapped {

    public:
        Mapped() = default;
        ~Mapped();

        Mapped(const Mapped &) = delete;
        Mapped(Mapped &&other) noexcept;
        Mapped &operator=(const Mapped &) = delete;
        Mapped &operator=(Mapped &&other) noexcept;

        static bool open(const std::filesystem::path &file, Mapped *out, std::string *error = nullptr);

        [[nodiscard]] std::span<const std::uint8_t> data() const { return {_data, _size}; }
        [[nodiscard]] std::size_t size() const { return _size; }
        [[nodiscard]] bool empty() const { return _size == 0; }

    private:
        void release();

        const std::uint8_t *_data = nullptr;
        std::size_t _size = 0;
    };
}


#endif //TIV_IMAGE_MAPPED_H
