// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_TILECODEC_H
#define TIV_IMAGE_TILECODEC_H


#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "image/Bitmap.h"

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace tiv::TileCodec {
    // A tile's pixels, lossless and quick to unpack, as a pyramid stores them. Either:
    // - planes: each channel on its own, red and blue less green for sRGB, the WebP lossless
    //   subtract green transform (RFC 9649, 4.2: https://www.rfc-editor.org/rfc/rfc9649.html). Each
    //   row less the row above, and when it leaves less, less its left neighbour too: PNG's Up, or its
    //   gradient without Paeth's choice, picked per row by the smallest sum of absolute differences as
    //   PNG suggests (http://www.libpng.org/pub/png/book/chapter09.html). Planes follow Zpng
    //   (https://github.com/catid/Zpng, BSD-3-Clause), then zstd.
    // - a palette: a tile of 256 colours or fewer as a byte a pixel and the colours, unfiltered, as PNG
    //   advises for indices.
    // PQ words are split into their four bytes as planes, without the green.

    struct ContextFree {
        void operator()(ZSTD_CCtx_s *context) const;
        void operator()(ZSTD_DCtx_s *context) const;
    };

    // Reused tile after tile, one per thread.
    class Encoder {

    public:
        Encoder();

        // The tile's rows of RGBA, pitch bytes apart, kept as 3 channels dropping alpha, or 4. Returns
        // the packed bytes, valid until the next call, or empty on failure.
        [[nodiscard]] std::span<const std::uint8_t> encode(const std::uint8_t *rows, std::size_t pitch, int width,
                                                           int height, int channels, Bitmap::Encoding encoding);

    private:
        std::span<const std::uint8_t> planes(const std::uint8_t *rows, std::size_t pitch, int width, int height,
                                             int channels, Bitmap::Encoding encoding);
        // The colours and indices count_colours() left.
        std::span<const std::uint8_t> palette();

        std::unique_ptr<ZSTD_CCtx_s, ContextFree> _context;
        std::vector<std::uint8_t> _planes;
        std::vector<std::uint8_t> _residuals;
        std::vector<std::uint32_t> _colours;
        std::vector<std::uint8_t> _indices;
        std::vector<std::uint8_t> _packed;
        std::vector<std::uint8_t> _other;
    };

    class Decoder {

    public:
        Decoder();

        // Unpacks into the tile, RGBA of the width and height, from the channels it was packed as.
        bool decode(std::span<const std::uint8_t> packed, int channels, Bitmap::Encoding encoding, Bitmap *tile);

    private:
        bool palette(std::span<const std::uint8_t> packed, int channels, Bitmap *tile);
        bool planes(std::span<const std::uint8_t> packed, int channels, Bitmap::Encoding encoding, Bitmap *tile);

        std::unique_ptr<ZSTD_DCtx_s, ContextFree> _context;
        std::vector<std::uint8_t> _planes;
    };

    // The most a tile of the size packs to, for checking what a file says.
    [[nodiscard]] std::size_t bound(int width, int height, int channels);
}


#endif //TIV_IMAGE_TILECODEC_H
