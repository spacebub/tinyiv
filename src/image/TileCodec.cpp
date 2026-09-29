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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <zstd.h>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/TileCodec.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): planes and rows are walked by offset.
namespace tiv::TileCodec {
    namespace {
        enum class Mode : std::uint8_t {
            Planes = 0,
            Palette = 1,
        };

        // zstd's fastest level that keeps most of the ratio.
        constexpr int ZSTD_LEVEL = 1;
        constexpr std::size_t MAX_COLOURS = 256;
        // Twice the colours, so probing stays short.
        constexpr std::size_t SLOTS = 512;
        constexpr std::uint32_t OPAQUE = 0xFF000000U;

        [[nodiscard]] std::size_t filter_bytes(const int height) {
            return (static_cast<std::size_t>(height) + 7) / 8;
        }

        [[nodiscard]] bool green_apart(const int channels, const Bitmap::Encoding encoding) {
            return channels >= 3 && encoding == Bitmap::Encoding::Srgb;
        }

        [[nodiscard]] int magnitude(const std::uint8_t residual) {
            return std::abs(static_cast<int>(static_cast<std::int8_t>(residual)));
        }

        // The colours of the tile and each pixel's index into them, or false past MAX_COLOURS.
        bool count_colours(const std::uint8_t *rows, const std::size_t pitch, const int width, const int height,
                           std::vector<std::uint32_t> &colours, std::vector<std::uint8_t> &indices) {
            std::array<std::uint32_t, SLOTS> keys{};
            std::array<std::int16_t, SLOTS> slots{};

            slots.fill(-1);
            colours.clear();
            indices.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

            std::uint8_t *index = indices.data();

            for (int y = 0; y < height; ++y) {
                const std::uint8_t *row = rows + (pitch * static_cast<std::size_t>(y));

                for (int x = 0; x < width; ++x) {
                    std::uint32_t colour = 0;

                    std::memcpy(&colour, row + (static_cast<std::size_t>(x) * Bitmap::CHANNELS), sizeof colour);

                    // Fibonacci hashing, the top bits of the product.
                    std::size_t slot = (colour * 2654435761U) >> 23U;

                    while (slots.at(slot) >= 0 && keys.at(slot) != colour) {
                        slot = (slot + 1) % SLOTS;
                    }

                    if (slots.at(slot) < 0) {
                        if (colours.size() == MAX_COLOURS) {
                            return false;
                        }

                        keys.at(slot) = colour;
                        slots.at(slot) = static_cast<std::int16_t>(colours.size());
                        colours.push_back(colour);
                    }

                    *index++ = static_cast<std::uint8_t>(slots.at(slot));
                }
            }

            return true;
        }

        // RGBA rows into planes of the channels, red and blue less green when it is apart.
        void split(const std::uint8_t *rows, const std::size_t pitch, const std::size_t width, const int height,
                   const std::size_t channels, const bool green, std::uint8_t *planes) {
            const std::size_t area = width * static_cast<std::size_t>(height);

            for (int y = 0; y < height; ++y) {
                const std::uint8_t *row = rows + (pitch * static_cast<std::size_t>(y));
                std::uint8_t *line = planes + (static_cast<std::size_t>(y) * width);

                for (std::size_t c = 0; c < channels; ++c) {
                    std::uint8_t *plane = line + (c * area);

                    for (std::size_t x = 0; x < width; ++x) {
                        plane[x] = row[(x * Bitmap::CHANNELS) + c];
                    }
                }
            }

            if (green) {
                std::uint8_t *red = planes;
                const std::uint8_t *middle = planes + area;
                std::uint8_t *blue = planes + (2 * area);

                for (std::size_t i = 0; i < area; ++i) {
                    red[i] = static_cast<std::uint8_t>(red[i] - middle[i]);
                    blue[i] = static_cast<std::uint8_t>(blue[i] - middle[i]);
                }
            }
        }

        // One row of every plane less the row above, and less its left neighbour too when that
        // leaves less. True when it does.
        bool filter_row(const std::uint8_t *planes, const std::size_t width, const std::size_t area, const int y,
                        const std::size_t channels, std::uint8_t *residuals) {
            const std::size_t line = static_cast<std::size_t>(y) * width;
            long up = 0;
            long gradient = 0;

            for (std::size_t c = 0; c < channels; ++c) {
                const std::uint8_t *plane = planes + (c * area) + line;
                std::uint8_t *out = residuals + (c * area) + line;
                std::uint8_t before = 0;

                for (std::size_t x = 0; x < width; ++x) {
                    const auto residual = static_cast<std::uint8_t>(plane[x] - (y > 0 ? plane[x - width] : 0));

                    out[x] = residual;
                    up += magnitude(residual);
                    gradient += magnitude(static_cast<std::uint8_t>(residual - before));
                    before = residual;
                }
            }

            if (gradient >= up) {
                return false;
            }

            for (std::size_t c = 0; c < channels; ++c) {
                std::uint8_t *out = residuals + (c * area) + line;

                // Right to left, so each left neighbour is still the row above's residual.
                for (std::size_t x = width - 1; x > 0; --x) {
                    out[x] = static_cast<std::uint8_t>(out[x] - out[x - 1]);
                }
            }

            return true;
        }
    }

    void ContextFree::operator()(ZSTD_CCtx *context) const {
        ZSTD_freeCCtx(context);
    }

    void ContextFree::operator()(ZSTD_DCtx *context) const {
        ZSTD_freeDCtx(context);
    }

    std::size_t bound(const int width, const int height, const int channels) {
        const std::size_t area = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t head = 2 + std::max(filter_bytes(height), MAX_COLOURS * sizeof(std::uint32_t));

        return head + ZSTD_compressBound(area * static_cast<std::size_t>(channels));
    }

    Encoder::Encoder() : _context(ZSTD_createCCtx()) {
    }

    std::span<const std::uint8_t> Encoder::encode(const std::uint8_t *rows, const std::size_t pitch, const int width,
                                                  const int height, const int channels,
                                                  const Bitmap::Encoding encoding) {
        if (_context == nullptr) {
            return {};
        }

        const std::span<const std::uint8_t> filtered = planes(rows, pitch, width, height, channels, encoding);

        // Few colours can still be a smooth ramp the filters do better on, so both are tried.
        if (count_colours(rows, pitch, width, height, _colours, _indices)) {
            const std::span<const std::uint8_t> indexed = palette();

            if (!indexed.empty() && (filtered.empty() || indexed.size() < filtered.size())) {
                return indexed;
            }
        }

        return filtered;
    }

    std::span<const std::uint8_t> Encoder::planes(const std::uint8_t *rows, const std::size_t pitch, const int width,
                                                  const int height, const int channels,
                                                  const Bitmap::Encoding encoding) {
        const auto w = static_cast<std::size_t>(width);
        const std::size_t area = w * static_cast<std::size_t>(height);
        const auto count = static_cast<std::size_t>(channels);
        const std::size_t head = 1 + filter_bytes(height);

        _planes.resize(area * count);
        _residuals.resize(area * count);
        split(rows, pitch, w, height, count, green_apart(channels, encoding), _planes.data());

        _packed.assign(head, 0);
        _packed.at(0) = static_cast<std::uint8_t>(Mode::Planes);

        std::uint8_t *filters = _packed.data() + 1;

        for (int y = 0; y < height; ++y) {
            if (filter_row(_planes.data(), w, area, y, count, _residuals.data())) {
                filters[y / 8] |= static_cast<std::uint8_t>(1U << (static_cast<unsigned>(y) % 8U));
            }
        }

        _packed.resize(head + ZSTD_compressBound(_residuals.size()));

        const std::size_t made = ZSTD_compressCCtx(_context.get(), _packed.data() + head, _packed.size() - head,
                                                   _residuals.data(), _residuals.size(), ZSTD_LEVEL);

        return ZSTD_isError(made) != 0 ? std::span<const std::uint8_t>() : std::span(_packed).first(head + made);
    }

    std::span<const std::uint8_t> Encoder::palette() {
        const std::size_t head = 2 + (_colours.size() * sizeof(std::uint32_t));

        _other.resize(head + ZSTD_compressBound(_indices.size()));
        _other.at(0) = static_cast<std::uint8_t>(Mode::Palette);
        _other.at(1) = static_cast<std::uint8_t>(_colours.size() - 1);
        std::memcpy(_other.data() + 2, _colours.data(), _colours.size() * sizeof(std::uint32_t));

        const std::size_t made = ZSTD_compressCCtx(_context.get(), _other.data() + head, _other.size() - head,
                                                   _indices.data(), _indices.size(), ZSTD_LEVEL);

        return ZSTD_isError(made) != 0 ? std::span<const std::uint8_t>() : std::span(_other).first(head + made);
    }

    Decoder::Decoder() : _context(ZSTD_createDCtx()) {
    }

    bool Decoder::decode(const std::span<const std::uint8_t> packed, const int channels,
                         const Bitmap::Encoding encoding, Bitmap *tile) {
        if (_context == nullptr || packed.size() < 2 || (channels != 3 && channels != Bitmap::CHANNELS)) {
            return false;
        }

        switch (static_cast<Mode>(packed.front())) {
            case Mode::Palette:
                return palette(packed, channels, tile);
            case Mode::Planes:
                return planes(packed, channels, encoding, tile);
        }

        return false;
    }

    bool Decoder::palette(const std::span<const std::uint8_t> packed, const int channels, Bitmap *tile) {
        const std::size_t area = static_cast<std::size_t>(tile->width()) * static_cast<std::size_t>(tile->height());
        const std::size_t colours = static_cast<std::size_t>(packed[1]) + 1;
        const std::size_t head = 2 + (colours * sizeof(std::uint32_t));
        std::array<std::uint32_t, MAX_COLOURS> palette{};

        if (packed.size() < head) {
            return false;
        }

        std::memcpy(palette.data(), packed.data() + 2, colours * sizeof(std::uint32_t));
        _planes.resize(area);

        if (ZSTD_decompressDCtx(_context.get(), _planes.data(), area, packed.data() + head, packed.size() - head)
            != area) {
            return false;
        }

        const std::uint8_t *index = _planes.data();
        std::uint8_t *out = tile->data();
        const std::uint32_t opaque = channels == 3 ? OPAQUE : 0U;

        for (std::size_t i = 0; i < area; ++i) {
            const std::uint32_t colour = palette.at(index[i]) | opaque;

            std::memcpy(out + (i * Bitmap::CHANNELS), &colour, sizeof colour);
        }

        return true;
    }

    bool Decoder::planes(const std::span<const std::uint8_t> packed, const int channels,
                         const Bitmap::Encoding encoding, Bitmap *tile) {
        const auto w = static_cast<std::size_t>(tile->width());
        const int height = tile->height();
        const std::size_t area = w * static_cast<std::size_t>(height);
        const auto count = static_cast<std::size_t>(channels);
        const std::size_t head = 1 + filter_bytes(height);

        _planes.resize(area * count);

        if (packed.size() < head
            || ZSTD_decompressDCtx(_context.get(), _planes.data(), _planes.size(), packed.data() + head,
                                   packed.size() - head)
                       != _planes.size()) {
            return false;
        }

        const std::uint8_t *filters = packed.data() + 1;
        const bool green = green_apart(channels, encoding);
        std::uint8_t *planes = _planes.data();

        for (int y = 0; y < height; ++y) {
            const std::size_t line = static_cast<std::size_t>(y) * w;
            const bool gradient = ((filters[y / 8] >> (static_cast<unsigned>(y) % 8U)) & 1U) != 0;

            for (std::size_t c = 0; c < count; ++c) {
                std::uint8_t *row = planes + (c * area) + line;

                if (gradient) {
                    Channels::prefix(row, w);
                }

                if (y > 0) {
                    Channels::accumulate(row, row - w, w);
                }
            }

            const std::uint8_t *red = planes + line;

            Channels::interleave(red, red + area, red + (2 * area),
                                 count == Bitmap::CHANNELS ? red + (3 * area) : nullptr, green, tile->row(y).data(), w);
        }

        return true;
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
