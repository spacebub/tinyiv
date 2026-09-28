// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "image/Exif.h"

namespace tiv {
    namespace {
        constexpr std::uint16_t TAG_ORIENTATION = 0x0112;
        constexpr std::size_t ENTRY_BYTES = 12;
        constexpr std::size_t HEADER_BYTES = 8;

        struct Reader {
            std::span<const std::uint8_t> block;
            bool little = true;

            [[nodiscard]] std::uint16_t u16(const std::size_t at) const {
                if (at + 2 > block.size()) {
                    return 0;
                }

                return little ? static_cast<std::uint16_t>(block[at]
                                                           | (static_cast<std::uint32_t>(block[at + 1]) << 8U))
                              : static_cast<std::uint16_t>((static_cast<std::uint32_t>(block[at]) << 8U)
                                                           | block[at + 1]);
            }

            [[nodiscard]] std::uint32_t u32(const std::size_t at) const {
                if (at + 4 > block.size()) {
                    return 0;
                }

                return little ? static_cast<std::uint32_t>(u16(at)) | (static_cast<std::uint32_t>(u16(at + 2)) << 16U)
                              : (static_cast<std::uint32_t>(u16(at)) << 16U) | static_cast<std::uint32_t>(u16(at + 2));
            }
        };
    }

    namespace {
        constexpr std::uint16_t TYPE_SHORT = 3;
        constexpr std::uint16_t MAGIC = 0x2A;

        struct Writer {
            bool little = true;

            void u16(std::vector<std::uint8_t> &out, const std::uint16_t value) const {
                const auto low = static_cast<std::uint8_t>(value & 0xFFU);
                const auto high = static_cast<std::uint8_t>(value >> 8U);

                out.push_back(little ? low : high);
                out.push_back(little ? high : low);
            }

            void u32(std::vector<std::uint8_t> &out, const std::uint32_t value) const {
                u16(out, static_cast<std::uint16_t>(little ? value & 0xFFFFU : value >> 16U));
                u16(out, static_cast<std::uint16_t>(little ? value >> 16U : value & 0xFFFFU));
            }
        };

        std::vector<std::uint8_t> orientation_entry(const Writer &writer, const int orientation) {
            std::vector<std::uint8_t> entry;

            writer.u16(entry, TAG_ORIENTATION);
            writer.u16(entry, TYPE_SHORT);
            writer.u32(entry, 1);
            writer.u16(entry, static_cast<std::uint16_t>(orientation));
            writer.u16(entry, 0);

            return entry;
        }
    }

    int Exif::orientation(std::span<const std::uint8_t> block) {
        constexpr std::size_t PREFIX = 6;

        if (block.size() >= PREFIX && block[0] == 'E' && block[1] == 'x' && block[2] == 'i' && block[3] == 'f') {
            block = block.subspan(PREFIX);
        }

        if (block.size() < HEADER_BYTES) {
            return 1;
        }

        Reader reader{.block = block, .little = true};

        if (block[0] == 'M' && block[1] == 'M') {
            reader.little = false;
        } else if (block[0] != 'I' || block[1] != 'I') {
            return 1;
        }

        if (reader.u16(2) != 0x2A) {
            return 1;
        }

        const std::size_t ifd = reader.u32(4);

        if (ifd + 2 > block.size()) {
            return 1;
        }

        const std::size_t count = reader.u16(ifd);

        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t entry = ifd + 2 + (i * ENTRY_BYTES);

            if (entry + ENTRY_BYTES > block.size()) {
                return 1;
            }

            if (reader.u16(entry) != TAG_ORIENTATION) {
                continue;
            }

            const int value = reader.u16(entry + 8);

            return value >= 1 && value <= 8 ? value : 1;
        }

        return 1;
    }

    bool Exif::orient(const std::span<const std::uint8_t> block, const int orientation, std::vector<Splice> *out) {
        out->clear();

        if (block.size() < HEADER_BYTES || block.size() > UINT32_MAX) {
            return false;
        }

        Reader reader{.block = block, .little = true};

        if (block[0] == 'M' && block[1] == 'M') {
            reader.little = false;
        } else if (block[0] != 'I' || block[1] != 'I') {
            return false;
        }

        const Writer writer{.little = reader.little};
        const std::size_t ifd = reader.u32(4);

        if (reader.u16(2) != MAGIC || ifd < HEADER_BYTES || ifd + 2 > block.size()) {
            return false;
        }

        const std::size_t count = reader.u16(ifd);
        const std::size_t entries = ifd + 2;
        const std::size_t end = entries + (count * ENTRY_BYTES) + 4;

        if (end > block.size()) {
            return false;
        }

        std::size_t found = count;
        std::size_t insert = count;

        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t tag = reader.u16(entries + (i * ENTRY_BYTES));

            if (tag == TAG_ORIENTATION) {
                found = i;
            }

            if (tag >= TAG_ORIENTATION && insert == count) {
                insert = i;
            }
        }

        const auto raw = [&](const std::size_t from, const std::size_t to) { return block.subspan(from, to - from); };
        const auto header = [&](const std::size_t at) {
            std::vector<std::uint8_t> bytes;

            writer.u32(bytes, static_cast<std::uint32_t>(at));

            return Splice{.at = 4, .length = 4, .bytes = bytes};
        };

        if (found == count) {
            if (orientation == 1) {
                return true;
            }

            // TIFF 6.0, section 2: the entries of an IFD are sorted in ascending order by tag.
            std::vector<std::uint8_t> copy;
            const std::vector<std::uint8_t> added = orientation_entry(writer, orientation);
            const std::size_t split = entries + (insert * ENTRY_BYTES);

            writer.u16(copy, static_cast<std::uint16_t>(count + 1));
            std::ranges::copy(raw(entries, split), std::back_inserter(copy));
            std::ranges::copy(added, std::back_inserter(copy));
            std::ranges::copy(raw(split, end), std::back_inserter(copy));

            out->push_back(header(block.size()));
            out->push_back({.at = block.size(), .length = 0, .bytes = std::move(copy)});

            return true;
        }

        const std::size_t entry = entries + (found * ENTRY_BYTES);

        if (reader.u16(entry + 2) != TYPE_SHORT || reader.u32(entry + 4) != 1) {
            return false;
        }

        // IFD0 at the very end, with the IFD it was copied from still in place without the tag.
        if (orientation == 1 && end == block.size()) {
            std::vector<std::uint8_t> original;

            writer.u16(original, static_cast<std::uint16_t>(count - 1));
            std::ranges::copy(raw(entries, entry), std::back_inserter(original));
            std::ranges::copy(raw(entry + ENTRY_BYTES, end), std::back_inserter(original));

            const std::span<const std::uint8_t> before = raw(HEADER_BYTES, ifd);
            const auto at = std::search(before.begin(), before.end(),
                                        std::boyer_moore_horspool_searcher(original.begin(), original.end()));

            if (at != before.end()) {
                out->push_back(header(HEADER_BYTES + static_cast<std::size_t>(at - before.begin())));
                out->push_back({.at = ifd, .length = block.size() - ifd, .bytes = {}});

                return true;
            }
        }

        if (std::cmp_not_equal(reader.u16(entry + 8), orientation)) {
            std::vector<std::uint8_t> value;

            writer.u16(value, static_cast<std::uint16_t>(orientation));
            out->push_back({.at = entry + 8, .length = 2, .bytes = std::move(value)});
        }

        return true;
    }

    std::vector<std::uint8_t> Exif::minimal(const int orientation) {
        const Writer writer{.little = true};
        std::vector<std::uint8_t> block = {'I', 'I'};

        writer.u16(block, MAGIC);
        writer.u32(block, HEADER_BYTES);
        writer.u16(block, 1);
        std::ranges::copy(orientation_entry(writer, orientation), std::back_inserter(block));
        writer.u32(block, 0);

        return block;
    }

    std::vector<std::uint8_t> Exif::apply(const std::span<const std::uint8_t> data,
                                          const std::vector<Splice> &splices) {
        std::vector<std::uint8_t> held;
        std::size_t from = 0;

        for (const Splice &splice : splices) {
            held.insert(held.end(), data.begin() + static_cast<std::ptrdiff_t>(from),
                        data.begin() + static_cast<std::ptrdiff_t>(splice.at));
            held.insert(held.end(), splice.bytes.begin(), splice.bytes.end());
            from = splice.at + splice.length;
        }

        held.insert(held.end(), data.begin() + static_cast<std::ptrdiff_t>(from), data.end());

        return held;
    }
}
