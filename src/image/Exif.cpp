// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <cstdint>
#include <span>

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

                return little ? static_cast<std::uint16_t>(block[at] | (block[at + 1] << 8))
                              : static_cast<std::uint16_t>((block[at] << 8) | block[at + 1]);
            }

            [[nodiscard]] std::uint32_t u32(const std::size_t at) const {
                if (at + 4 > block.size()) {
                    return 0;
                }

                return little ? static_cast<std::uint32_t>(u16(at)) | (static_cast<std::uint32_t>(u16(at + 2)) << 16)
                              : (static_cast<std::uint32_t>(u16(at)) << 16) | static_cast<std::uint32_t>(u16(at + 2));
            }
        };
    }

    int Exif::orientation(std::span<const std::uint8_t> block) {
        constexpr std::size_t PREFIX = 6;

        if (block.size() >= PREFIX && block[0] == 'E' && block[1] == 'x' && block[2] == 'i' && block[3] == 'f') {
            block = block.subspan(PREFIX);
        }

        if (block.size() < HEADER_BYTES) {
            return 1;
        }

        Reader reader{block, true};

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
}
