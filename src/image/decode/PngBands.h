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
#include <optional>
#include <span>
#include <vector>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"

namespace tiv::Decode {
// A PNG has no way in but the top, so one pass finds places to begin inflating again. Each keeps the inflate state,
// as Mark Adler's zran.c does for gzip (https://github.com/madler/zlib/blob/develop/examples/zran.c, zlib licence),
// and the row above for the filters. Any band of rows decodes from the nearest one, and bands decode side by side.
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
    // Count rows as RGBA, from y.
    using Take = std::function<bool(int y, int count, std::span<const std::uint8_t> pixels)>;

    // Null unless the file is a PNG this reads: not interlaced, 8 or 16 bits, grey, RGB or 8 bit
    // palette, with or without alpha. HDR rows are tone mapped for the display as the streamed
    // decode maps them, and an HDR palette or colour key is left to that decode.
    [[nodiscard]] static std::unique_ptr<PngBands> open(const std::filesystem::path &file,
                                                        const Tone::Display &display = {});

    [[nodiscard]] int width() const { return _width; }
    [[nodiscard]] int height() const { return _height; }
    [[nodiscard]] bool alpha() const;
    // PQ when an HDR image is shown on an HDR display.
    [[nodiscard]] Bitmap::Encoding encoding() const { return _encoding; }

    // Every row top to bottom, in bands of the rows given. A checkpoint goes at the first block that
    // starts past each stretch of about CHECKPOINT_BYTES inflated.
    bool scan(int rows, const Take &take, const Keep &keep, const Abort *abort);

    [[nodiscard]] std::span<const Checkpoint> checkpoints() const { return _checkpoints; }

    // The checkpoints of an earlier scan of the same file. False when they could not have come from one.
    bool adopt(std::vector<Checkpoint> checkpoints);

    // Count rows from top, or up to the bottom, as RGBA into out, which is as wide as the image.
    // They decode from the checkpoints before them, on up to threads threads.
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
    // True when this reads the layout. The chunks before the image data are given for the tone.
    [[nodiscard]] bool supported(std::span<const std::uint8_t> start, bool plain) const;

    // An unfiltered row to RGBA, as the PNG decoder of tinyiv has libpng make it. HDR goes through
    // the tone mapper, with the samples widened in the scratch.
    void expand(const std::uint8_t *row, std::uint8_t *out, std::vector<std::uint16_t> &wide) const;
    void expand_mapped(const Tone::Mapper &mapper,
                       const std::uint8_t *row,
                       std::uint8_t *out,
                       std::vector<std::uint16_t> &wide) const;
    void expand_grey(const std::uint8_t *row, std::uint8_t *out) const;
    void expand_colour(const std::uint8_t *row, std::uint8_t *out) const;

    // Inflates from the checkpoint, keeping rows from first to last as RGBA in out.
    bool decode_from(std::size_t checkpoint, int first, int last, Bitmap *out, int outTop, const Recall &recall) const;

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
    // For an HDR image, as Vips::prepare() makes it for the streamed decode.
    std::optional<Tone::Mapper> _mapper;
    Bitmap::Encoding _encoding = Bitmap::Encoding::Srgb;
    std::vector<Checkpoint> _checkpoints;
};
}


#endif //TIV_DECODE_PNGBANDS_H
