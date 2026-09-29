// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_JPEGBANDS_H
#define TIV_DECODE_JPEGBANDS_H


#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

namespace tiv::Decode {
    // The places in a baseline JPEG where its restart markers let decoding begin on a row, found by
    // one pass over the file. Any band of rows then decodes on its own, and bands decode side by side.
    // A restart resets the DC predictors and byte aligns the data, ITU-T T.81 F.1.2.3 and F.2.2.5:
    // https://www.w3.org/Graphics/JPEG/itu-t81.pdf
    // The OpenSlide Hamamatsu driver reads its JPEGs the same way, a small JPEG made per band from the
    // header and the data between two markers:
    // https://github.com/openslide/openslide/blob/main/src/openslide-vendor-hamamatsu.c (LGPL-2.1)
    class JpegBands {

    public:
        // Decoding can only begin this near to a row, else the bands are too coarse to be worth it.
        static constexpr int MAX_STEP = 1024;

        // False unless the data is one interleaved scan of 8 bit Huffman coded baseline in grey, YCbCr
        // or RGB, with restart markers that fall on rows at most MAX_STEP apart.
        [[nodiscard]] static bool index(std::span<const std::uint8_t> data, JpegBands *out);

        // As index(), taking the starts from an earlier index() of the same data instead of a pass over
        // it. False when they do not fit the data.
        [[nodiscard]] static bool adopt(std::span<const std::uint8_t> data, std::vector<std::uint64_t> starts,
                                        JpegBands *out);

        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }

        // Rows between two places decoding can begin.
        [[nodiscard]] int step() const { return _step; }

        // Where in the data each step's rows begin.
        [[nodiscard]] std::span<const std::uint64_t> starts() const { return _starts; }

        // The rows from top, count of them or to the bottom, scaled by num eighths, into out, which
        // is as wide as the scaled image and holds at least the scaled rows. Uses up to threads threads.
        // The top has to be a multiple of 8, where a scaled row begins.
        bool decode(std::span<const std::uint8_t> data, int top, int count, unsigned num, Bitmap *out, int threads,
                    const Abort *abort) const;

        // The scaled image's rows, as libjpeg counts them.
        [[nodiscard]] static int scaled(int rows, unsigned num);

    private:
        // Decodes the steps from first to last, not including last, keeping the scaled rows from
        // keepTop to keepBottom, where outTop is out's first row, and dropping the rest.
        bool decode_steps(std::span<const std::uint8_t> data, int first, int last, unsigned num, int keepTop,
                          int keepBottom, int outTop, Bitmap *out, const Abort *abort) const;

        struct Frame {
            int width = 0;
            int height = 0;
            int components = 0;
            int widest = 1;
            int tallest = 1;
            // A component sampled fewer times down than the tallest.
            bool subsampled = false;
        };

        // Reads the header, and all but the starts, from the data.
        [[nodiscard]] bool parse(std::span<const std::uint8_t> data);
        [[nodiscard]] static bool read_frame(std::span<const std::uint8_t> segment, Frame *out);
        // Sizes the MCUs, steps and intervals from the frame.
        [[nodiscard]] bool lay_out(const Frame &frame, std::uint32_t interval, std::uint64_t scan);

        [[nodiscard]] int steps() const;

        // SOI and the markers a decoder needs, up to and with SOS. The rest of the header is dropped.
        std::vector<std::uint8_t> _header;
        // Where the image height sits in the header.
        std::size_t _heightAt = 0;
        // Where the entropy coded data begins, just past SOS, and ends, at EOI.
        std::uint64_t _scan = 0;
        std::uint64_t _end = 0;
        std::vector<std::uint64_t> _starts;
        int _width = 0;
        int _height = 0;
        int _step = 0;
        int _mcuRows = 0;
        // MCU rows in a step, and the restart intervals in one and in the whole image.
        int _stepMcus = 0;
        std::uint64_t _stepIntervals = 0;
        std::uint64_t _intervals = 0;
        // Chroma is upsampled from the rows either side, so a band needs a step of margin each way
        // to decode its edges as the whole image would.
        bool _margins = false;
    };
}


#endif //TIV_DECODE_JPEGBANDS_H
