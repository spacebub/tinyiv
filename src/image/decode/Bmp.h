// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_BMP_H
#define TIV_DECODE_BMP_H


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

namespace tiv::Bmp {

    enum class Kernel : std::uint8_t {
        Auto,
        Scalar,
        Avx2,
    };

    // True when the kernel can run on this machine.
    [[nodiscard]] bool supports(Kernel kernel);

    // A Windows bitmap read from its headers, pointing into the data it was read from, which
    // has to outlive it.
    class Image {

    public:
        // A whole .bmp file.
        static bool open(std::span<const std::uint8_t> file, Image *out);

        // An ICO entry: the bitmap without its file header, a transparency mask below the pixels.
        static bool open_icon(std::span<const std::uint8_t> entry, Image *out);

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }

        // Every row as RGBA8, averaged factor by factor when factor is above one. Rows missing
        // from a truncated file come out transparent.
        bool decode(int factor, Bitmap *out, const Decode::Abort *abort = nullptr, Kernel kernel = Kernel::Auto) const;

    private:
        enum class Layout : std::uint8_t {
            Indexed,
            Rle8,
            Rle4,
            Bgr,
            Bgra,
            Masked16,
            Masked32,
        };

        enum class Alpha : std::uint8_t {
            Opaque,
            Pixels,
            // The transparency mask of an ICO entry.
            Mask,
        };

        struct Channel {
            std::uint32_t shift = 0;
            std::uint32_t low = 0;
            // From the channel's own width to eight bits.
            std::array<std::uint8_t, 256> scale{};
        };

        struct Kernels;

        static Kernels pick(Kernel kernel, bool stream);

        bool read(std::span<const std::uint8_t> data, std::size_t header, std::size_t pixelsAt, bool icon);
        bool choose_layout(std::uint32_t compression, std::array<std::uint32_t, 4> &masks, bool &alphaInPixels);
        // Returns where the table ends.
        std::size_t read_palette(std::span<const std::uint8_t> data, std::size_t palette, std::size_t end, std::size_t entry, std::uint32_t used);
        void find_mask();
        void set_masks(const std::array<std::uint32_t, 4> &masks);
        [[nodiscard]] bool any_alpha(std::uint32_t mask) const;

        [[nodiscard]] std::span<const std::uint8_t> stored(int y) const;
        void row(int y, std::span<std::uint8_t> out, const Kernels &kernels) const;
        void indexed_row(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) const;
        void masked_row(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) const;
        // Hands out every row bottom first, an empty one for a row left transparent.
        bool unpack_rle(const std::function<void(std::span<const std::uint8_t>)> &emit, const Decode::Abort *abort) const;
        bool decode_rle(int factor, Bitmap &target, const Decode::Abort *abort) const;
        void decode_band(int from, int to, int factor, Bitmap &target, const Kernels &kernels, const Decode::Abort *abort, std::atomic<bool> &stopped) const;

        std::span<const std::uint8_t> _pixels;
        std::span<const std::uint8_t> _mask;
        // RGBA per entry, the bitmap's own order.
        std::array<std::uint8_t, std::size_t{256} * 4> _palette{};
        std::array<Channel, 4> _channels{};
        std::size_t _stride = 0;
        std::size_t _maskStride = 0;
        int _width = 0;
        int _height = 0;
        int _depth = 0;
        bool _bottomUp = true;
        Layout _layout = Layout::Bgr;
        Alpha _alpha = Alpha::Opaque;
    };

}


#endif //TIV_DECODE_BMP_H
