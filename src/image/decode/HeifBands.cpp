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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <libheif/heif.h>
#include <libheif/heif_properties.h>
#include <libheif/heif_tiling.h>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/FileReader.h"
#include "image/Mapped.h"
#include "image/decode/Decode.h"
#include "image/decode/Heif.h"
#include "image/decode/HeifBands.h"
#include "image/decode/Support.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): tiles are copied row by row.
namespace tiv::Decode {
    namespace {
        // What libheif reads the file through, so a path of any characters opens on every system.
        struct Stream {
            FileReader file;
            std::uint64_t at = 0;

            explicit Stream(const std::filesystem::path &path) : file(path) {}
        };

        Stream &stream_of(void *user) {
            return *static_cast<Stream *>(user);
        }

        constexpr heif_reader READER{
                .reader_api_version = 1,
                .get_position = [](void *user) { return static_cast<std::int64_t>(stream_of(user).at); },
                .read =
                        [](void *data, const std::size_t size, void *user) {
                            Stream &stream = stream_of(user);
                            const std::size_t got =
                                    stream.file.read(stream.at, static_cast<std::uint8_t *>(data), size);

                            stream.at += got;

                            return got == size ? 0 : -1;
                        },
                .seek =
                        [](const std::int64_t position, void *user) {
                            stream_of(user).at = static_cast<std::uint64_t>(position);

                            return 0;
                        },
                .wait_for_file_size =
                        [](const std::int64_t target, void *user) {
                            return static_cast<std::uint64_t>(target) <= stream_of(user).file.size()
                                           ? heif_reader_grow_status_size_reached
                                           : heif_reader_grow_status_size_beyond_eof;
                        },
                .request_range = nullptr,
                .preload_range_hint = nullptr,
                .release_file_range = nullptr,
                .release_error_msg = nullptr,
        };
    }

    struct HeifBands::Opened {
        Stream stream;
        heif_context *context = heif_context_alloc();
        heif_image_handle *handle = nullptr;
        heif_decoding_options *options = heif_decoding_options_alloc();

        explicit Opened(const std::filesystem::path &file) : stream(file) {}

        ~Opened() {
            heif_decoding_options_free(options);

            if (handle != nullptr) {
                heif_image_handle_release(handle);
            }

            heif_context_free(context);
        }

        Opened(const Opened &) = delete;
        Opened(Opened &&) = delete;
        Opened &operator=(const Opened &) = delete;
        Opened &operator=(Opened &&) = delete;

        // The file's primary image, or null when libheif cannot read it.
        [[nodiscard]] static std::unique_ptr<Opened> of(const std::filesystem::path &file) {
            auto opened = std::make_unique<Opened>(file);

            if (!opened->stream.file.valid() || opened->context == nullptr || opened->options == nullptr
                || heif_context_read_from_reader(opened->context, &READER, &opened->stream, nullptr).code
                           != heif_error_Ok
                || heif_context_get_primary_image_handle(opened->context, &opened->handle).code != heif_error_Ok) {
                return nullptr;
            }

            return opened;
        }

        // Turned, mirrored or cropped, which libvips may lay out otherwise than the tiles do.
        [[nodiscard]] bool transformed() const {
            return heif_item_get_transformation_properties(context, heif_image_handle_get_item_id(handle), nullptr, 0)
                   > 0;
        }
    };

    std::unique_ptr<HeifBands> HeifBands::open(const std::filesystem::path &file) {
        std::unique_ptr<Opened> opened = Opened::of(file);

        if (opened == nullptr || opened->transformed()) {
            return nullptr;
        }

        heif_image_tiling tiling{};
        const heif_image_handle *handle = opened->handle;
        if (heif_image_handle_get_image_tiling(handle, 1, &tiling).code != heif_error_Ok
            || tiling.num_columns * tiling.num_rows < 2 || tiling.top_offset != 0 || tiling.left_offset != 0
            || tiling.number_of_extra_dimensions != 0 || tiling.tile_width == 0 || tiling.tile_height == 0
            || heif_image_handle_get_luma_bits_per_pixel(handle) != 8
            || heif_image_handle_get_chroma_bits_per_pixel(handle) != 8
            // A tile decoded on its own comes without the alpha, which lies in grids of its own.
            || heif_image_handle_has_alpha_channel(handle) != 0) {
            return nullptr;
        }

        // An HDR image is tone mapped by the decode through libvips, which the tiles are not.
        if (Mapped mapped;
            !Mapped::open(file, &mapped, nullptr, Mapped::Use::Scattered) || Heif::colour(mapped.data()).tone.hdr()) {
            return nullptr;
        }

        std::unique_ptr<HeifBands> held(new HeifBands());

        held->_file = file;
        held->_width = heif_image_handle_get_width(handle);
        held->_height = heif_image_handle_get_height(handle);
        held->_columns = static_cast<int>(tiling.num_columns);
        held->_rows = static_cast<int>(tiling.num_rows);
        held->_tileWidth = static_cast<int>(tiling.tile_width);
        held->_tileHeight = static_cast<int>(tiling.tile_height);
        held->give(std::move(opened));

        return held->_width > 0 && held->_height > 0 ? std::move(held) : nullptr;
    }

    HeifBands::~HeifBands() = default;

    std::unique_ptr<HeifBands::Opened> HeifBands::take() const {
        {
            const std::scoped_lock hold(_guard);

            if (!_idle.empty()) {
                std::unique_ptr<Opened> opened = std::move(_idle.back());

                _idle.pop_back();

                return opened;
            }
        }

        return Opened::of(_file);
    }

    void HeifBands::give(std::unique_ptr<Opened> opened) const {
        const std::scoped_lock hold(_guard);

        _idle.push_back(std::move(opened));
    }

    bool HeifBands::read(const Opened &opened, const int column, const int row, const int top, const int bottom,
                         Bitmap *out) const {
        heif_image *image = nullptr;
        const heif_error error = heif_image_handle_decode_image_tile(
                opened.handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGB, opened.options,
                static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(row));

        if (error.code != heif_error_Ok || image == nullptr) {
            return false;
        }

        std::size_t stride = 0;
        const std::uint8_t *pixels = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
        const int x = column * _tileWidth;
        const int y = row * _tileHeight;
        // The last column and row may be cut to the image.
        const int width = std::min({heif_image_get_width(image, heif_channel_interleaved), _tileWidth, _width - x});
        const int height = std::min({heif_image_get_height(image, heif_channel_interleaved), _tileHeight, _height - y});
        const bool ok = pixels != nullptr && stride >= static_cast<std::size_t>(width) * 3;

        for (int line = std::max(y, top); ok && line < std::min(y + height, bottom); ++line) {
            const std::uint8_t *from = pixels + (stride * static_cast<std::size_t>(line - y));
            std::uint8_t *to = out->row(line - top).subspan(static_cast<std::size_t>(x) * Bitmap::CHANNELS).data();

            Channels::expand(from, to, width);
        }

        heif_image_release(image);

        return ok;
    }

    bool HeifBands::decode(const int top, const int count, Bitmap *out, const int threads, const Abort *abort) const {
        const int bottom = std::min(top + count, _height);

        if (top < 0 || bottom <= top || out->width() != _width || out->height() < bottom - top) {
            return false;
        }

        struct Tile {
            int column = 0;
            int row = 0;
        };

        std::vector<Tile> tiles;

        for (int row = top / _tileHeight; row < _rows && row * _tileHeight < bottom; ++row) {
            for (int column = 0; column < _columns; ++column) {
                tiles.push_back({.column = column, .row = row});
            }
        }

        std::atomic<std::size_t> next = 0;
        std::atomic<bool> ok = true;

        const auto work = [&] {
            std::unique_ptr<Opened> opened = take();

            if (opened == nullptr) {
                ok = false;

                return;
            }

            for (std::size_t i = next++; i < tiles.size() && ok && !aborted(abort); i = next++) {
                if (!read(*opened, tiles.at(i).column, tiles.at(i).row, top, bottom, out)) {
                    ok = false;
                }
            }

            give(std::move(opened));
        };

        {
            std::vector<std::jthread> workers;
            const auto spread = std::min(static_cast<std::size_t>(std::max(threads, 1)), tiles.size());

            for (std::size_t i = 1; i < spread; ++i) {
                workers.emplace_back(work);
            }

            work();
        }

        return ok && !aborted(abort);
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
