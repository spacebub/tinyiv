// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_PNGBANDS_H
#define TIV_DECODE_PNGBANDS_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

namespace tiv::Decode {
    // A PNG, which has no way in but the top, given places to begin inflating again, found on one pass
    // over it. At each, the state inflate needs is kept, as Mark Adler's zran.c does for gzip
    // (https://github.com/madler/zlib/blob/develop/examples/zran.c, zlib licence), with the row above
    // for the filters, so any band of rows decodes from the nearest one and bands decode side by side.
    class PngBands {

    public:
        // Where inflating can begin again, a deflate block's start.
        struct Checkpoint {
            // The next compressed byte in the file, and what is left of its IDAT chunk.
            std::uint64_t file = 0;
            std::uint32_t left = 0;
            // Bits of the byte before still to be read, and that byte.
            std::uint8_t bits = 0;
            std::uint8_t prime = 0;
            // Inflated bytes before it, and the first whole row after it.
            std::uint64_t out = 0;
            std::uint32_t first = 0;
        };

        // The window of inflated bytes before a checkpoint and the unfiltered row above its first,
        // kept by whoever holds the checkpoints.
        using Keep = std::function<bool(std::size_t checkpoint, std::span<const std::uint8_t> state)>;
        using Recall = std::function<bool(std::size_t checkpoint, std::vector<std::uint8_t> &state)>;
        // Rows as RGBA from y, count of them.
        using Take = std::function<bool(int y, int count, std::span<const std::uint8_t> pixels)>;

        // Null unless the file is a PNG this reads: not interlaced, 8 or 16 bits, grey, RGB or 8 bit
        // palette, with or without alpha, and SDR.
        [[nodiscard]] static std::unique_ptr<PngBands> open(const std::filesystem::path &file);

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }
        [[nodiscard]] bool alpha() const;

        // Every row top to bottom, in bands of the rows given, a checkpoint at the first block to
        // start past each stretch of about CHECKPOINT_BYTES inflated.
        bool scan(int rows, const Take &take, const Keep &keep, const Abort *abort);

        [[nodiscard]] std::span<const Checkpoint> checkpoints() const { return _checkpoints; }

        // The checkpoints of an earlier scan of the same file.
        void adopt(std::vector<Checkpoint> checkpoints) { _checkpoints = std::move(checkpoints); }

        // The rows from top, count of them or to the bottom, as RGBA into out, which is as wide as
        // the image, from the checkpoints before them. Uses up to threads threads.
        bool decode(int top, int count, Bitmap *out, int threads, const Recall &recall) const;

        // Inflated bytes between checkpoints, so a stretch of rows decodes in a few milliseconds.
        static constexpr std::uint64_t CHECKPOINT_BYTES = std::uint64_t{8} << 20U;
        static constexpr std::size_t WINDOW = std::size_t{32} * 1024;

    private:
        // Inflated bytes cut into scanlines and unfiltered, row after row.
        struct Lines;
        // What a scan() carries from one inflate to the next.
        struct Scan;

        PngBands() = default;

        // What a chunk before the image data says. Plain is cleared by an interlaced image.
        void take_chunk(std::span<const std::uint8_t> type, std::span<const std::uint8_t> body, bool *plain);
        // The layout is one this reads, the chunks before the image data given for the tone.
        [[nodiscard]] bool supported(std::span<const std::uint8_t> start, bool plain) const;

        // An unfiltered row to RGBA, as tinyiv's PNG decoder has libpng make it.
        void expand(const std::uint8_t *row, std::uint8_t *out) const;
        void expand_grey(const std::uint8_t *row, std::uint8_t *out) const;
        void expand_colour(const std::uint8_t *row, std::uint8_t *out) const;

        // Inflates from the checkpoint, keeping rows from first to last as RGBA in out.
        bool decode_from(std::size_t checkpoint, int first, int last, Bitmap *out, int outTop,
                         const Recall &recall) const;

        std::filesystem::path _file;
        int _width = 0;
        int _height = 0;
        int _depth = 8;
        std::uint8_t _colour = 2;
        // Bytes a pixel as filtered, at least one, and a scanline without its filter byte.
        int _filterStep = 1;
        std::size_t _rowBytes = 0;
        // The first IDAT's data and its length.
        std::uint64_t _data = 0;
        std::uint32_t _length = 0;
        // RGBA of each palette entry, and the colour tRNS makes transparent for grey and RGB.
        std::array<std::uint32_t, 256> _palette{};
        std::array<std::uint16_t, 3> _key{};
        bool _keyed = false;
        std::vector<Checkpoint> _checkpoints;
    };
}


#endif //TIV_DECODE_PNGBANDS_H
