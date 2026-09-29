// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
// jpeglib.h names FILE without including it.
#include <cstdio>
#include <cstring>
#include <numeric>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <jpeglib.h>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "image/decode/JpegBands.h"
#include "image/decode/JpegHandle.h"
#include "image/decode/Support.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): markers are found by offset into the data.
namespace tiv::Decode {
    namespace {
        constexpr std::uint8_t MARK = 0xFF;
        constexpr std::uint8_t SOI = 0xD8;
        constexpr std::uint8_t EOI = 0xD9;
        constexpr std::uint8_t RST0 = 0xD0;
        constexpr std::uint8_t RST7 = 0xD7;
        constexpr std::uint8_t SOF0 = 0xC0;
        constexpr std::uint8_t SOF1 = 0xC1;
        constexpr std::uint8_t DHT = 0xC4;
        constexpr std::uint8_t DQT = 0xDB;
        constexpr std::uint8_t DRI = 0xDD;
        constexpr std::uint8_t SOS = 0xDA;
        constexpr std::uint8_t APP0 = 0xE0;
        // Adobe's, which says whether three channels are YCbCr or RGB.
        constexpr std::uint8_t APP14 = 0xEE;
        constexpr int BLOCK = 8;
        // Each band pays for its own header, and a margin when chroma is subsampled, so a thread
        // takes at least this many rows.
        constexpr int MIN_ROWS = 64;

        // Rows libjpeg hands back at once, which it keeps to the tallest sampling factor.
        constexpr int MAX_BATCH = 16;
        constexpr std::array<std::uint8_t, 2> END = {0xFF, 0xD9};

        [[nodiscard]] bool is_restart(const std::uint8_t marker) {
            return marker >= RST0 && marker <= RST7;
        }

        // Feeds libjpeg its parts one after another without joining them: the header made for a band,
        // then the band's data where it lies in the file, then EOI.
        // As "Compressed data handling (source and destination managers)" in libjpeg.txt describes:
        // https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/doc/libjpeg.txt
        class Chain {

        public:
            explicit Chain(const std::array<std::span<const std::uint8_t>, 3> parts) : _parts(parts) {
                _source.init_source = [](j_decompress_ptr /*info*/) {};
                _source.fill_input_buffer = fill;
                _source.skip_input_data = skip;
                _source.resync_to_restart = resync;
                _source.term_source = [](j_decompress_ptr /*info*/) {};
            }

            [[nodiscard]] jpeg_source_mgr *source() { return &_source; }

        private:
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the source manager is the first member.
            static Chain &of(j_decompress_ptr info) { return *reinterpret_cast<Chain *>(info->src); }

            static boolean fill(j_decompress_ptr info) {
                Chain &chain = of(info);

                while (chain._next < chain._parts.size() && chain._parts.at(chain._next).empty()) {
                    ++chain._next;
                }

                // Past the end, as libjpeg's own sources do: a fake EOI ends the image.
                const std::span<const std::uint8_t> part =
                        chain._next < chain._parts.size() ? chain._parts.at(chain._next++) : std::span(END);

                info->src->next_input_byte = part.data();
                info->src->bytes_in_buffer = part.size();

                return TRUE;
            }

            static void skip(j_decompress_ptr info, long bytes) {
                while (std::cmp_greater(bytes, info->src->bytes_in_buffer)) {
                    bytes -= static_cast<long>(info->src->bytes_in_buffer);
                    fill(info);
                }

                if (bytes > 0) {
                    info->src->next_input_byte += bytes;
                    info->src->bytes_in_buffer -= static_cast<std::size_t>(bytes);
                }
            }

            // The band starts at some restart in the file, not at RST0, so whichever comes is taken as
            // the one expected. The markers are still checked to be restarts.
            static boolean resync(j_decompress_ptr info, const int desired) {
                if (is_restart(static_cast<std::uint8_t>(info->unread_marker))) {
                    info->unread_marker = 0;

                    return TRUE;
                }

                return jpeg_resync_to_restart(info, desired);
            }

            jpeg_source_mgr _source{};
            std::array<std::span<const std::uint8_t>, 3> _parts;
            std::size_t _next = 0;
        };

        [[nodiscard]] std::uint32_t big16(const std::span<const std::uint8_t> data, const std::size_t at) {
            return (static_cast<std::uint32_t>(data[at]) << 8U) | data[at + 1];
        }

        // What the decoder needs of the header. The rest, Exif and ICC among it, can be large.
        [[nodiscard]] bool kept(const std::uint8_t marker) {
            return marker == SOF0 || marker == SOF1 || marker == DHT || marker == DQT || marker == DRI || marker == APP0
                   || marker == APP14 || marker == SOS;
        }

        // Every SOF but the two plain Huffman sequential ones is progressive, lossless, arithmetic or
        // hierarchical, none of which restart markers can split this way.
        [[nodiscard]] bool other_frame(const std::uint8_t marker) {
            constexpr std::uint8_t SOF15 = 0xCF;
            constexpr std::uint8_t JPG = 0xC8;

            return marker >= SOF0 && marker <= SOF15 && marker != SOF0 && marker != SOF1 && marker != DHT
                   && marker != JPG;
        }

        // The marker segment at the offset, past any fill bytes, and the offset moved beyond it.
        [[nodiscard]] bool next_segment(const std::span<const std::uint8_t> data, std::size_t *at,
                                        std::span<const std::uint8_t> *out) {
            while (*at + 1 < data.size() && data[*at] == MARK && data[*at + 1] == MARK) {
                ++*at;
            }

            if (*at + 4 > data.size() || data[*at] != MARK) {
                return false;
            }

            const std::size_t length = big16(data, *at + 2);

            if (length < 2 || *at + 2 + length > data.size() || other_frame(data[*at + 1])) {
                return false;
            }

            *out = data.subspan(*at, 2 + length);
            *at += out->size();

            return true;
        }
    }

    bool JpegBands::read_frame(const std::span<const std::uint8_t> segment, Frame *out) {
        constexpr std::size_t PRECISION = 4;
        constexpr std::size_t COMPONENTS = 9;
        constexpr std::size_t HEADER = 10;

        if (segment.size() < HEADER || segment[PRECISION] != BLOCK) {
            return false;
        }

        out->height = static_cast<int>(big16(segment, 5));
        out->width = static_cast<int>(big16(segment, 7));
        out->components = segment[COMPONENTS];

        if ((out->components != 1 && out->components != 3)
            || segment.size() < HEADER + (3 * static_cast<std::size_t>(out->components))) {
            return false;
        }

        std::array<int, 3> tall{};

        for (int i = 0; i < out->components; ++i) {
            const auto at = static_cast<std::size_t>(i);
            const std::uint8_t factors = segment[HEADER + 1 + (3 * at)];

            out->widest = std::max(out->widest, static_cast<int>(factors >> 4U));
            tall.at(at) = static_cast<int>(factors & 0x0FU);
            out->tallest = std::max(out->tallest, tall.at(at));
        }

        out->subsampled = std::ranges::any_of(std::span(tall).first(static_cast<std::size_t>(out->components)),
                                              [&](const int v) { return v < out->tallest; });

        return true;
    }

    int JpegBands::scaled(const int rows, const unsigned num) {
        return static_cast<int>(((static_cast<std::uint64_t>(rows) * num) + BLOCK - 1) / BLOCK);
    }

    int JpegBands::steps() const {
        return (_mcuRows + _stepMcus - 1) / _stepMcus;
    }

    bool JpegBands::parse(const std::span<const std::uint8_t> data) {
        if (data.size() < 4 || data[0] != MARK || data[1] != SOI) {
            return false;
        }

        _header.assign({MARK, SOI});

        std::size_t at = 2;
        std::uint32_t interval = 0;
        Frame frame;

        for (;;) {
            std::span<const std::uint8_t> segment;

            if (!next_segment(data, &at, &segment)) {
                return false;
            }

            const std::uint8_t marker = segment[1];
            const std::size_t length = segment.size() - 2;

            if ((marker == SOF0 || marker == SOF1) && !read_frame(segment, &frame)) {
                return false;
            }

            if (marker == SOF0 || marker == SOF1) {
                _heightAt = _header.size() + 5;
            }

            if (marker == DRI && length >= 4) {
                interval = big16(segment, 4);
            }

            if (kept(marker)) {
                _header.insert(_header.end(), segment.begin(), segment.end());
            }


            // One scan of every component, else the rows are spread over several scans.
            if (marker == SOS) {
                if (frame.components == 0 || std::cmp_not_equal(segment[4], frame.components)) {
                    return false;
                }

                break;
            }
        }

        return lay_out(frame, interval, at);
    }

    bool JpegBands::lay_out(const Frame &frame, const std::uint32_t interval, const std::uint64_t scan) {
        // A single component scan has one block to an MCU, whatever its sampling.
        const int mcuWidth = frame.components == 1 ? BLOCK : BLOCK * frame.widest;
        const int mcuHeight = frame.components == 1 ? BLOCK : BLOCK * frame.tallest;

        if (frame.width == 0 || frame.height == 0 || interval == 0) {
            return false;
        }

        const auto perRow = static_cast<std::uint64_t>((frame.width + mcuWidth - 1) / mcuWidth);

        _width = frame.width;
        _height = frame.height;
        _mcuRows = (_height + mcuHeight - 1) / mcuHeight;
        _stepMcus = static_cast<int>(interval / std::gcd(static_cast<std::uint64_t>(interval), perRow));
        _step = _stepMcus * mcuHeight;
        _stepIntervals = static_cast<std::uint64_t>(_stepMcus) * perRow / interval;
        _intervals = ((perRow * static_cast<std::uint64_t>(_mcuRows)) + interval - 1) / interval;
        _margins = frame.components == 3 && frame.subsampled;
        _scan = scan;

        return _step <= MAX_STEP;
    }

    bool JpegBands::index(const std::span<const std::uint8_t> data, JpegBands *out) {
        JpegBands held;

        if (!held.parse(data)) {
            return false;
        }

        held._starts.reserve(static_cast<std::size_t>(held.steps()));
        held._starts.push_back(held._scan);

        std::uint64_t restarts = 0;
        const std::uint8_t *at = data.data() + held._scan;
        const std::uint8_t *last = data.data() + data.size() - 1;

        while (at < last) {
            const auto *found =
                    static_cast<const std::uint8_t *>(std::memchr(at, MARK, static_cast<std::size_t>(last - at)));

            if (found == nullptr) {
                break;
            }

            const std::uint8_t next = found[1];

            // A stuffed zero is data, and a second 0xFF is fill before a marker.
            if (next == 0x00 || next == MARK) {
                at = found + (next == 0x00 ? 2 : 1);

                continue;
            }

            if (next == EOI) {
                held._end = static_cast<std::uint64_t>(found - data.data());

                break;
            }

            // Anything else is a second scan or a DNL, which this cannot follow.
            if (!is_restart(next)) {
                return false;
            }

            ++restarts;

            if (restarts % held._stepIntervals == 0) {
                held._starts.push_back(static_cast<std::uint64_t>(found + 2 - data.data()));
            }

            at = found + 2;
        }

        if (held._end == 0 || restarts + 1 != held._intervals
            || held._starts.size() != static_cast<std::size_t>(held.steps())) {
            return false;
        }

        *out = std::move(held);

        return true;
    }

    bool JpegBands::adopt(const std::span<const std::uint8_t> data, std::vector<std::uint64_t> starts, JpegBands *out) {
        JpegBands held;

        if (!held.parse(data) || starts.size() != static_cast<std::size_t>(held.steps())
            || starts.front() != held._scan) {
            return false;
        }

        // Only ordered and within the file: reading each marker would touch the whole file again.
        for (std::size_t i = 1; i < starts.size(); ++i) {
            if (starts.at(i) <= starts.at(i - 1) || starts.at(i) >= data.size()) {
                return false;
            }
        }

        // The end lies within the last step's data, so only that is searched.
        for (std::uint64_t at = starts.back(); at + 1 < data.size(); ++at) {
            if (data[at] == MARK && data[at + 1] == EOI) {
                held._end = at;

                break;
            }
        }

        if (held._end == 0) {
            return false;
        }

        held._starts = std::move(starts);
        *out = std::move(held);

        return true;
    }

    bool JpegBands::decode_steps(const Fetch &fetch, const int first, const int last, const unsigned num,
                                 const int keepTop, const int keepBottom, const int outTop, Bitmap *out,
                                 const Abort *abort) const {
        const int top = first * _step;
        const int rows = std::min(last * _step, _height) - top;
        const std::uint64_t from = _starts.at(static_cast<std::size_t>(first));
        const std::uint64_t to = last < steps() ? _starts.at(static_cast<std::size_t>(last)) - 2 : _end;
        std::vector<std::uint8_t> header(_header);

        header.at(_heightAt) = static_cast<std::uint8_t>(static_cast<unsigned>(rows) >> 8U);
        header.at(_heightAt + 1) = static_cast<std::uint8_t>(static_cast<unsigned>(rows) & 0xFFU);

        std::vector<std::uint8_t> scratch;
        const std::span<const std::uint8_t> scan = fetch(from, to - from, scratch);

        if (scan.size() != to - from) {
            return false;
        }

        Chain chain({header, scan, std::span(END)});
        JpegHandle handle;
        std::vector<std::uint8_t> spill(static_cast<std::size_t>(out->width()) * Bitmap::CHANNELS);
        std::array<JSAMPROW, MAX_BATCH> targets{};

        // NOLINTNEXTLINE(cert-err52-cpp,modernize-avoid-setjmp-longjmp,cppcoreguidelines-pro-bounds-array-to-pointer-decay): libjpeg reports errors by longjmp only.
        if (setjmp(handle.error.jump) != 0) {
            return false;
        }

        jpeg_create_decompress(&handle.info);
        handle.created = true;
        handle.info.src = chain.source();

        if (jpeg_read_header(&handle.info, TRUE) != JPEG_HEADER_OK) {
            return false;
        }

        handle.info.scale_num = num;
        handle.info.scale_denom = BLOCK;
        handle.info.out_color_space = JCS_EXT_RGBA;
        handle.info.dct_method = JDCT_ISLOW;

        if (jpeg_start_decompress(&handle.info) == 0 || static_cast<int>(handle.info.output_width) != out->width()
            || handle.info.rec_outbuf_height > MAX_BATCH) {
            return false;
        }

        const int base = scaled(top, num);
        const auto batch = static_cast<std::size_t>(std::max(handle.info.rec_outbuf_height, 1));

        // The margin above only has to be entropy decoded, skipping spares its transforms.
        if (const int above = keepTop - base; above > 0) {
            jpeg_skip_scanlines(&handle.info, static_cast<JDIMENSION>(above));
        }

        // Rows below keepBottom are only margin, and the decoder has read what it needs of them.
        while (static_cast<int>(handle.info.output_scanline) + base < keepBottom) {
            const int at = static_cast<int>(handle.info.output_scanline) + base;

            if (at % ABORT_ROWS == 0 && aborted(abort)) {
                return false;
            }

            for (std::size_t i = 0; i < batch; ++i) {
                const int row = at + static_cast<int>(i);

                targets.at(i) = row >= keepTop && row < keepBottom ? out->row(row - outTop).data() : spill.data();
            }

            if (jpeg_read_scanlines(&handle.info, targets.data(), static_cast<JDIMENSION>(batch)) == 0) {
                return false;
            }
        }

        jpeg_abort_decompress(&handle.info);

        return true;
    }

    bool JpegBands::decode(const std::span<const std::uint8_t> data, const int top, const int count, const unsigned num,
                           Bitmap *out, const int threads, const Abort *abort) const {
        const Fetch lying = [&](const std::uint64_t offset, const std::uint64_t bytes,
                                std::vector<std::uint8_t> & /*scratch*/) {
            return offset + bytes <= data.size() ? data.subspan(offset, bytes) : std::span<const std::uint8_t>();
        };

        return decode(lying, top, count, num, out, threads, abort);
    }

    bool JpegBands::decode(const Fetch &fetch, const int top, const int count, const unsigned num, Bitmap *out,
                           const int threads, const Abort *abort) const {
        const int bottom = std::min(top + count, _height);

        if (top < 0 || top % BLOCK != 0 || bottom <= top || out->width() != scaled(_width, num)
            || out->height() < scaled(bottom, num) - scaled(top, num)) {
            return false;
        }

        const int first = top / _step;
        const int last = (bottom + _step - 1) / _step;
        const int margin = _margins ? 1 : 0;
        const int spread = std::max(1, std::min(threads, (last - first) / ((MIN_ROWS + _step - 1) / _step)));
        const int per = (last - first + spread - 1) / spread;
        const int outTop = scaled(top, num);
        std::atomic<bool> ok = true;

        const auto band = [&](const int from) {
            const int to = std::min(from + per, last);

            const int keepTop = std::max(scaled(from * _step, num), outTop);
            const int keepBottom = scaled(std::min(to * _step, bottom), num);

            if (!decode_steps(fetch, std::max(from - margin, 0), std::min(to + margin, steps()), num, keepTop,
                              keepBottom, outTop, out, abort)) {
                ok = false;
            }
        };

        {
            std::vector<std::jthread> workers;

            for (int from = first + per; from < last; from += per) {
                workers.emplace_back(band, from);
            }

            band(first);
        }

        return ok && !aborted(abort);
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
