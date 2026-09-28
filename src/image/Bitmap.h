// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_BITMAP_H
#define TIV_IMAGE_BITMAP_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace tiv {
    // Four bytes a pixel, rows packed with no padding. The buffer is 64 byte aligned and starts
    // out uninitialised: a decoder overwrites every byte, and zeroing 700 MB first is a
    // measurable cost.
    class Bitmap {

    public:
        static constexpr int CHANNELS = 4;
        static constexpr std::size_t ALIGNMENT = 64;
        static constexpr std::uint32_t PQ_MASK = 0x3FF;

        enum class Encoding : std::uint8_t {
            // RGBA8 sRGB.
            Srgb,
            // HDR for an HDR display: one 32 bit word a pixel of BT.2020 in PQ, 10 bits each
            // of red from the lowest, green and blue, then 2 of alpha.
            Pq,
        };

        Bitmap() = default;

        static Bitmap allocate(int width, int height, Encoding encoding = Encoding::Srgb);

        [[nodiscard]] Encoding encoding() const { return _encoding; }

        // For pixels rewritten in place into another encoding, which takes the same four bytes.
        void set_encoding(const Encoding encoding) { _encoding = encoding; }

        // The channels of a Pq word, red first, alpha from 0 to 3.
        [[nodiscard]] static constexpr std::array<std::uint32_t, 4> unpack(const std::uint32_t word) {
            return {word & PQ_MASK, (word >> 10U) & PQ_MASK, (word >> 20U) & PQ_MASK, word >> 30U};
        }

        [[nodiscard]] static constexpr std::uint32_t pack(const std::uint32_t red, const std::uint32_t green,
                                                          const std::uint32_t blue, const std::uint32_t alpha) {
            return red | (green << 10U) | (blue << 20U) | (alpha << 30U);
        }

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }
        [[nodiscard]] bool empty() const { return _pixels == nullptr; }

        [[nodiscard]] std::size_t pitch() const { return static_cast<std::size_t>(_width) * CHANNELS; }
        [[nodiscard]] std::size_t bytes() const { return pitch() * static_cast<std::size_t>(_height); }
        [[nodiscard]] std::int64_t pixels() const { return static_cast<std::int64_t>(_width) * _height; }

        [[nodiscard]] const std::uint8_t *data() const { return _pixels.get(); }
        [[nodiscard]] std::uint8_t *data() { return _pixels.get(); }

        [[nodiscard]] std::span<const std::uint8_t> row(const int y) const {
            return std::span(_pixels.get(), bytes()).subspan(pitch() * static_cast<std::size_t>(y), pitch());
        }

        [[nodiscard]] std::span<std::uint8_t> row(const int y) {
            return std::span(_pixels.get(), bytes()).subspan(pitch() * static_cast<std::size_t>(y), pitch());
        }

        [[nodiscard]] std::span<const std::uint8_t> all() const { return {_pixels.get(), bytes()}; }
        [[nodiscard]] std::span<std::uint8_t> all() { return {_pixels.get(), bytes()}; }

    private:
        struct Free {
            void operator()(std::uint8_t *memory) const;
        };

        int _width = 0;
        int _height = 0;
        Encoding _encoding = Encoding::Srgb;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): the array form is how unique_ptr owns a raw buffer.
        std::unique_ptr<std::uint8_t[], Free> _pixels;
    };
}


#endif //TIV_IMAGE_BITMAP_H
