// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <tiffio.h>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/decode/Decode.h"
#include "image/decode/Support.h"
#include "image/decode/TiffBands.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-vararg): libtiff's interface is C.
namespace tiv::Decode {
    namespace {
        // A strip or tile larger than this is not what a viewer should hold per thread.
        constexpr std::uint64_t MAX_UNIT_BYTES = std::uint64_t{1} << 30U;
        // Far past any image, and far enough from the limit of an int that sums of sides stay within it.
        constexpr std::uint32_t MAX_SIDE = std::uint32_t{1} << 30U;

        // Errors come back as failed reads, and libvips owns the global handlers, so each handle
        // is given its own that say nothing.
        int quiet(TIFF * /*handle*/, void * /*user*/, const char * /*module*/, const char * /*format*/,
                  va_list /*arguments*/) {
            return 1;
        }

        TIFF *open_handle(const std::filesystem::path &file) {
            TIFFOpenOptions *options = TIFFOpenOptionsAlloc();

            TIFFOpenOptionsSetErrorHandlerExtR(options, quiet, nullptr);
            TIFFOpenOptionsSetWarningHandlerExtR(options, quiet, nullptr);

            // Read, not mapped: a mapping of a file cut short under it faults.
#ifdef _WIN32
            TIFF *handle = TIFFOpenWExt(file.c_str(), "rm", options);
#else
            TIFF *handle = TIFFOpenExt(file.c_str(), "rm", options);
#endif
            TIFFOpenOptionsFree(options);

            return handle;
        }

        // The one extra sample is a straight alpha, as libvips reads it too.
        bool plain_alpha(TIFF *handle) {
            std::uint16_t count = 0;
            std::uint16_t *kinds = nullptr;

            return TIFFGetField(handle, TIFFTAG_EXTRASAMPLES, &count, &kinds) == 1 && count == 1
                   && kinds[0] == EXTRASAMPLE_UNASSALPHA;
        }

        bool supported(TIFF *handle) {
            std::uint16_t bits = 0;
            std::uint16_t samples = 0;
            std::uint16_t format = SAMPLEFORMAT_UINT;
            std::uint16_t planar = PLANARCONFIG_CONTIG;
            std::uint16_t photometric = 0;
            std::uint16_t compression = COMPRESSION_NONE;

            TIFFGetFieldDefaulted(handle, TIFFTAG_BITSPERSAMPLE, &bits);
            TIFFGetFieldDefaulted(handle, TIFFTAG_SAMPLESPERPIXEL, &samples);
            TIFFGetFieldDefaulted(handle, TIFFTAG_SAMPLEFORMAT, &format);
            TIFFGetFieldDefaulted(handle, TIFFTAG_PLANARCONFIG, &planar);
            TIFFGetFieldDefaulted(handle, TIFFTAG_COMPRESSION, &compression);

            if (TIFFGetField(handle, TIFFTAG_PHOTOMETRIC, &photometric) != 1 || bits != 8 || format != SAMPLEFORMAT_UINT
                || planar != PLANARCONFIG_CONTIG || compression == COMPRESSION_OJPEG
                || TIFFIsCODECConfigured(compression) == 0) {
                return false;
            }

            const bool grey = photometric == PHOTOMETRIC_MINISBLACK && (samples == 1 || samples == 2);
            const bool colour = photometric == PHOTOMETRIC_RGB && (samples == 3 || samples == 4);
            const bool extra = samples == 2 || samples == 4;

            return (grey || colour) && (!extra || plain_alpha(handle));
        }

        // Samples of one pixel's worth, as the file has them, into RGBA.
        void expand(const std::uint8_t *from, const int samples, const int pixels, std::uint8_t *to) {
            constexpr std::uint8_t OPAQUE = 0xFF;

            switch (samples) {
                case 4:
                    std::memcpy(to, from, static_cast<std::size_t>(pixels) * Bitmap::CHANNELS);
                    break;
                case 3:
                    Channels::expand(from, to, pixels);
                    break;
                default:
                    for (int i = 0; i < pixels; ++i) {
                        const std::uint8_t grey = from[static_cast<std::size_t>(i) * static_cast<std::size_t>(samples)];
                        std::uint8_t *pixel = to + (static_cast<std::size_t>(i) * Bitmap::CHANNELS);

                        pixel[0] = grey;
                        pixel[1] = grey;
                        pixel[2] = grey;
                        pixel[3] = samples == 2 ? from[(static_cast<std::size_t>(i) * 2) + 1] : OPAQUE;
                    }

                    break;
            }
        }
    }

    std::unique_ptr<TiffBands> TiffBands::open(const std::filesystem::path &file) {
        TIFF *handle = open_handle(file);

        if (handle == nullptr) {
            return nullptr;
        }

        std::unique_ptr<TiffBands> held(new TiffBands());
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint16_t samples = 0;

        held->_file = file;
        held->_idle.push_back(handle);

        if (!supported(handle) || TIFFGetField(handle, TIFFTAG_IMAGEWIDTH, &width) != 1
            || TIFFGetField(handle, TIFFTAG_IMAGELENGTH, &height) != 1 || width == 0 || height == 0 || width > MAX_SIDE
            || height > MAX_SIDE) {
            return nullptr;
        }

        TIFFGetFieldDefaulted(handle, TIFFTAG_SAMPLESPERPIXEL, &samples);

        held->_width = static_cast<int>(width);
        held->_height = static_cast<int>(height);
        held->_samples = samples;
        held->_tiled = TIFFIsTiled(handle) != 0;

        std::uint64_t bytes = 0;

        if (held->_tiled) {
            std::uint32_t tileWidth = 0;
            std::uint32_t tileHeight = 0;

            if (TIFFGetField(handle, TIFFTAG_TILEWIDTH, &tileWidth) != 1
                || TIFFGetField(handle, TIFFTAG_TILELENGTH, &tileHeight) != 1 || tileWidth == 0 || tileHeight == 0) {
                return nullptr;
            }

            held->_unitWidth = static_cast<int>(tileWidth);
            held->_unitHeight = static_cast<int>(tileHeight);
            bytes = TIFFTileSize64(handle);
        } else {
            std::uint32_t rows = 0;

            TIFFGetFieldDefaulted(handle, TIFFTAG_ROWSPERSTRIP, &rows);
            held->_unitWidth = held->_width;
            held->_unitHeight = static_cast<int>(std::min(std::max(rows, 1U), height));
            bytes = TIFFStripSize64(handle);
        }

        const std::uint64_t expected =
                static_cast<std::uint64_t>(held->_unitWidth) * static_cast<std::uint64_t>(held->_unitHeight) * samples;

        if (bytes < expected || bytes > MAX_UNIT_BYTES) {
            return nullptr;
        }

        held->_unitBytes = static_cast<std::size_t>(bytes);

        return held;
    }

    TiffBands::~TiffBands() {
        for (TIFF *handle : _idle) {
            TIFFClose(handle);
        }
    }

    TIFF *TiffBands::take() const {
        {
            const std::scoped_lock hold(_guard);

            if (!_idle.empty()) {
                TIFF *handle = _idle.back();

                _idle.pop_back();

                return handle;
            }
        }

        return open_handle(_file);
    }

    void TiffBands::give(TIFF *handle) const {
        const std::scoped_lock hold(_guard);

        _idle.push_back(handle);
    }

    std::vector<TiffBands::Unit> TiffBands::units(const int top, const int bottom) const {
        std::vector<Unit> held;
        const int across = (_width + _unitWidth - 1) / _unitWidth;

        for (int row = top / _unitHeight; row * _unitHeight < bottom; ++row) {
            for (int column = 0; column < across; ++column) {
                held.push_back({
                        .x = column * _unitWidth,
                        .y = row * _unitHeight,
                        .index = static_cast<std::uint32_t>((row * across) + column),
                });
            }
        }

        return held;
    }

    bool TiffBands::read(TIFF *handle, const Unit &unit, const int top, const int bottom,
                         std::vector<std::uint8_t> &scratch, Bitmap *out) const {
        scratch.resize(_unitBytes);

        const tmsize_t got =
                _tiled ? TIFFReadEncodedTile(handle, unit.index, scratch.data(), static_cast<tmsize_t>(scratch.size()))
                       : TIFFReadEncodedStrip(handle, unit.index, scratch.data(),
                                              static_cast<tmsize_t>(scratch.size()));

        if (got < 0) {
            return false;
        }

        const int pixels = std::min(_unitWidth, _width - unit.x);
        const std::size_t pitch = static_cast<std::size_t>(_unitWidth) * static_cast<std::size_t>(_samples);
        const int last = std::min({unit.y + _unitHeight, bottom, _height});

        for (int y = std::max(unit.y, top); y < last; ++y) {
            expand(scratch.data() + (pitch * static_cast<std::size_t>(y - unit.y)), _samples, pixels,
                   out->row(y - top).subspan(static_cast<std::size_t>(unit.x) * Bitmap::CHANNELS).data());
        }

        return true;
    }

    bool TiffBands::decode(const int top, const int count, Bitmap *out, const int threads, const Abort *abort) const {
        const int bottom = std::min(top + count, _height);

        if (top < 0 || bottom <= top || out->width() != _width || out->height() < bottom - top) {
            return false;
        }

        const std::vector<Unit> todo = units(top, bottom);
        std::atomic<std::size_t> next = 0;
        std::atomic<bool> ok = true;

        const auto work = [&] {
            TIFF *handle = take();

            if (handle == nullptr) {
                ok = false;

                return;
            }

            std::vector<std::uint8_t> scratch;

            for (std::size_t i = next++; i < todo.size() && ok && !aborted(abort); i = next++) {
                if (!read(handle, todo.at(i), top, bottom, scratch, out)) {
                    ok = false;
                }
            }

            give(handle);
        };

        {
            std::vector<std::jthread> workers;
            const auto spread = std::min(static_cast<std::size_t>(std::max(threads, 1)), todo.size());

            for (std::size_t i = 1; i < spread; ++i) {
                workers.emplace_back(work);
            }

            work();
        }

        return ok && !aborted(abort);
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-vararg)
