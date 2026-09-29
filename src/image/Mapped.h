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

        // How the file will be read, which decides what the system reads ahead.
        enum class Use : std::uint8_t {
            // Front to back, as a decoder goes.
            Through,
            // Here and there, as an index points.
            Scattered,
        };

        static bool open(const std::filesystem::path &file, Mapped *out, std::string *error = nullptr,
                         Use use = Use::Through);

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
