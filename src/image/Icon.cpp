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
#include <span>
#include <string_view>
#include <utility>

#include "image/Bitmap.h"
#include "image/Icon.h"

namespace tiv {
    namespace {
        constexpr std::size_t ICO_HEADER = 6;
        constexpr std::size_t ICO_ENTRY = 16;
        constexpr std::size_t ICNS_HEADER = 8;
        constexpr std::size_t DIB_HEADER = 40;

        // Width and height sit at fixed offsets in IHDR, the first chunk: https://www.w3.org/TR/png-3/#11IHDR
        constexpr std::size_t PNG_IHDR = 16;

        // A run byte below this starts that many plus one literal bytes, one at or above it
        // repeats the next byte, three times for the smallest. The scheme and the ICNS types are
        // described at https://en.wikipedia.org/wiki/Apple_Icon_Image_format
        constexpr std::uint8_t PACKED_REPEAT = 0x80;
        constexpr std::size_t PACKED_SHORTEST = 3;

        // A bigger entry always wins, and among entries of one size the higher preference.
        constexpr long PREFERENCES = 64;

        // Keeps the score of an entry well inside 64 bits.
        constexpr int MAX_SIDE = 1 << 20;

        constexpr std::array<std::uint8_t, 8> PNG_MAGIC = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        constexpr std::array<std::uint8_t, 12> JP2_MAGIC = {0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', '\r', '\n', 0x87, '\n'};
        constexpr std::array<std::uint8_t, 4> J2K_MAGIC = {0xFF, 0x4F, 0xFF, 0x51};

        struct IcnsType {
            std::string_view tag;
            int size = 0;
            // The entry holding the alpha of a packed type. Empty for PNG or JPEG 2000 types.
            std::string_view mask;
        };

        constexpr std::array ICNS_TYPES = {
                IcnsType{"icp4", 16, {}},
                IcnsType{"icp5", 32, {}},
                IcnsType{"icp6", 64, {}},
                IcnsType{"ic07", 128, {}},
                IcnsType{"ic08", 256, {}},
                IcnsType{"ic09", 512, {}},
                IcnsType{"ic10", 1024, {}},
                IcnsType{"ic11", 32, {}},
                IcnsType{"ic12", 64, {}},
                IcnsType{"ic13", 256, {}},
                IcnsType{"ic14", 512, {}},
                IcnsType{"is32", 16, "s8mk"},
                IcnsType{"il32", 32, "l8mk"},
                IcnsType{"ih32", 48, "h8mk"},
                IcnsType{"it32", 128, "t8mk"},
        };

        std::uint32_t le16(const std::span<const std::uint8_t> data, const std::size_t at) {
            if (at + 2 > data.size()) {
                return 0;
            }

            return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8);
        }

        std::uint32_t le32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return le16(data, at) | (le16(data, at + 2) << 16);
        }

        std::uint32_t be32(const std::span<const std::uint8_t> data, const std::size_t at) {
            if (at + 4 > data.size()) {
                return 0;
            }

            return (static_cast<std::uint32_t>(data[at]) << 24) | (static_cast<std::uint32_t>(data[at + 1]) << 16) | (static_cast<std::uint32_t>(data[at + 2]) << 8)
                   | data[at + 3];
        }

        bool starts(const std::span<const std::uint8_t> data, const std::span<const std::uint8_t> magic) {
            return data.size() >= magic.size() && std::ranges::equal(data.first(magic.size()), magic);
        }

        bool tagged(const std::span<const std::uint8_t> data, const std::size_t at, const std::string_view tag) {
            return at + tag.size() <= data.size() && std::ranges::equal(data.subspan(at, tag.size()), tag, {}, {}, [](const char c) {
                       return static_cast<std::uint8_t>(c);
                   });
        }

        // False when the PNG is too short to hold its size.
        bool png_size(const std::span<const std::uint8_t> data, Icon::Entry *entry) {
            if (!starts(data, PNG_MAGIC) || data.size() < PNG_IHDR + 8) {
                return false;
            }

            entry->payload = Icon::Payload::Png;
            entry->width = static_cast<int>(be32(data, PNG_IHDR));
            entry->height = static_cast<int>(be32(data, PNG_IHDR + 4));

            return true;
        }

        long score(const Icon::Entry &entry, const int preference) {
            if (entry.width <= 0 || entry.height <= 0 || entry.width > MAX_SIDE || entry.height > MAX_SIDE) {
                return -1;
            }

            return (static_cast<long>(entry.width) * entry.height * PREFERENCES) + std::clamp(preference, 0, static_cast<int>(PREFERENCES) - 1);
        }

        // One colour plane of a packed entry, starting at at, which moves past it. False when
        // the data ends first.
        bool unpack_plane(const std::span<const std::uint8_t> data, std::size_t &at, const std::span<std::uint8_t> pixels, const std::size_t channel) {
            const std::size_t count = pixels.size() / Bitmap::CHANNELS;

            for (std::size_t done = 0; done < count;) {
                if (at >= data.size()) {
                    return false;
                }

                const std::uint8_t run = data[at++];
                const bool repeat = run >= PACKED_REPEAT;
                const std::size_t length = std::min(repeat ? run - PACKED_REPEAT + PACKED_SHORTEST : std::size_t{run} + 1, count - done);

                if (at + (repeat ? 1 : length) > data.size()) {
                    return false;
                }

                for (std::size_t i = 0; i < length; ++i) {
                    pixels[((done + i) * Bitmap::CHANNELS) + channel] = data[repeat ? at : at + i];
                }

                at += repeat ? 1 : length;
                done += length;
            }

            return true;
        }

        // Calls visit with the offset of every chunk's tag and the data after its header.
        template<typename Visit>
        void each_chunk(const std::span<const std::uint8_t> file, Visit visit) {
            const std::size_t end = std::min<std::size_t>(be32(file, 4), file.size());

            for (std::size_t at = ICNS_HEADER; at + ICNS_HEADER <= end;) {
                const std::size_t length = be32(file, at + 4);

                if (length < ICNS_HEADER || length > end - at) {
                    return;
                }

                visit(at, file.subspan(at + ICNS_HEADER, length - ICNS_HEADER));
                at += length;
            }
        }
    }

    // The directory layout follows https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)
    bool Icon::largest_ico(const std::span<const std::uint8_t> file, Entry *out) {
        if (file.size() < ICO_HEADER || le16(file, 0) != 0 || le16(file, 2) != 1) {
            return false;
        }

        const std::size_t count = le16(file, 4);
        long best = -1;

        for (std::size_t i = 0; i < count && ICO_HEADER + ((i + 1) * ICO_ENTRY) <= file.size(); ++i) {
            const std::size_t at = ICO_HEADER + (i * ICO_ENTRY);
            const std::size_t size = le32(file, at + 8);
            const std::size_t offset = le32(file, at + 12);

            if (size == 0 || offset > file.size() || size > file.size() - offset) {
                continue;
            }

            Entry entry;
            int depth = 0;

            entry.data = file.subspan(offset, size);

            if (png_size(entry.data, &entry)) {
                depth = 32;
            } else if (le32(entry.data, 0) >= DIB_HEADER) {
                entry.payload = Payload::Dib;
                entry.width = static_cast<int>(le32(entry.data, 4));
                // The height counts the transparency mask stored below the pixels.
                entry.height = static_cast<int>(le32(entry.data, 8)) / 2;
                depth = static_cast<int>(le16(entry.data, 14));
            } else {
                continue;
            }

            if (const long held = score(entry, depth); held > best) {
                best = held;
                *out = entry;
            }
        }

        return best >= 0;
    }

    bool Icon::largest_icns(const std::span<const std::uint8_t> file, Entry *out) {
        if (!tagged(file, 0, "icns")) {
            return false;
        }

        long best = -1;
        std::string_view mask;

        each_chunk(file, [&](const std::size_t at, std::span<const std::uint8_t> data) {
            const auto *const type = std::ranges::find_if(ICNS_TYPES, [&](const IcnsType &known) {
                return tagged(file, at, known.tag);
            });

            if (type == ICNS_TYPES.end()) {
                return;
            }

            Entry entry;
            int preference = 0;

            entry.width = type->size;
            entry.height = type->size;

            if (!type->mask.empty()) {
                // The largest packed type puts four zero bytes ahead of its planes.
                if (type->tag == "it32") {
                    data = data.subspan(std::min<std::size_t>(4, data.size()));
                }

                entry.payload = Payload::Packed;
                preference = 1;
            } else if (png_size(data, &entry)) {
                preference = 3;
            } else if (starts(data, JP2_MAGIC) || starts(data, J2K_MAGIC)) {
                entry.payload = Payload::Jpeg2000;
                preference = 2;
            } else {
                return;
            }

            entry.data = data;

            if (const long held = score(entry, preference); held > best) {
                best = held;
                mask = type->mask;
                *out = entry;
            }
        });

        if (best < 0) {
            return false;
        }

        if (!mask.empty()) {
            each_chunk(file, [&](const std::size_t at, const std::span<const std::uint8_t> data) {
                if (tagged(file, at, mask)) {
                    out->mask = data;
                }
            });
        }

        return true;
    }

    bool Icon::unpack(const Entry &entry, Bitmap *out) {
        const std::size_t count = static_cast<std::size_t>(entry.width) * static_cast<std::size_t>(entry.height);
        const std::span<const std::uint8_t> data = entry.data;
        Bitmap held = Bitmap::allocate(entry.width, entry.height);
        const std::span<std::uint8_t> pixels = held.all();

        if (data.size() == count * Bitmap::CHANNELS) {
            // Small entries are sometimes stored plain, as a pad byte and RGB per pixel.
            for (std::size_t i = 0; i < count; ++i) {
                std::ranges::copy(data.subspan((i * Bitmap::CHANNELS) + 1, 3), pixels.subspan(i * Bitmap::CHANNELS, 3).begin());
            }
        } else {
            std::size_t at = 0;

            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (!unpack_plane(data, at, pixels, channel)) {
                    return false;
                }
            }
        }

        const bool masked = entry.mask.size() >= count;

        for (std::size_t i = 0; i < count; ++i) {
            pixels[(i * Bitmap::CHANNELS) + 3] = masked ? entry.mask[i] : 0xFF;
        }

        *out = std::move(held);

        return true;
    }
}
