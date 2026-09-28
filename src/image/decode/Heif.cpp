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
#include <span>
#include <string_view>
#include <vector>

#include "image/Tone.h"
#include "image/decode/Heif.h"

namespace tiv {
    namespace {
        constexpr std::size_t HEADER = 8;
        constexpr std::size_t FULL_HEADER = 4;

        constexpr std::string_view APPLE_GAIN_MAP = "urn:com:apple:photo:2020:aux:hdrgainmap";

        struct Box {
            std::string_view type;
            std::span<const std::uint8_t> body;
        };

        std::uint32_t u16(const std::span<const std::uint8_t> data, const std::size_t at) {
            return at + 2 <= data.size() ? (static_cast<std::uint32_t>(data[at]) << 8U) | data[at + 1] : 0;
        }

        std::uint32_t u32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return at + 4 <= data.size() ? (u16(data, at) << 16U) | u16(data, at + 2) : 0;
        }

        std::string_view fourcc(const std::span<const std::uint8_t> data, const std::size_t at) {
            if (at + 4 > data.size()) {
                return {};
            }

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes are ASCII.
            return {reinterpret_cast<const char *>(data.subspan(at, 4).data()), 4};
        }

        // Up to the terminating zero, or the end.
        std::string_view text(const std::span<const std::uint8_t> data) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes are ASCII.
            return {reinterpret_cast<const char *>(data.data()),
                    static_cast<std::size_t>(std::ranges::find(data, 0) - data.begin())};
        }

        // The boxes one after another in the data, stopping at the first that does not fit.
        // Spec: ISO/IEC 14496-12, section 4.2 (Box, with largesize and size 0 to the end).
        std::vector<Box> boxes(const std::span<const std::uint8_t> data) {
            std::vector<Box> held;

            for (std::size_t at = 0; at + HEADER <= data.size();) {
                std::uint64_t size = u32(data, at);
                std::size_t header = HEADER;

                if (size == 1) {
                    size = (static_cast<std::uint64_t>(u32(data, at + HEADER)) << 32U) | u32(data, at + HEADER + 4);
                    header += 8;
                } else if (size == 0) {
                    size = data.size() - at;
                }

                if (size < header || size > data.size() - at) {
                    break;
                }

                held.push_back({
                        .type = fourcc(data, at + 4),
                        .body = data.subspan(at + header, static_cast<std::size_t>(size) - header),
                });
                at += static_cast<std::size_t>(size);
            }

            return held;
        }

        std::span<const std::uint8_t> find(const std::vector<Box> &within, const std::string_view type) {
            const auto found = std::ranges::find(within, type, &Box::type);

            return found != within.end() ? found->body : std::span<const std::uint8_t>{};
        }

        std::uint32_t primary_item(const std::span<const std::uint8_t> pitm) {
            return pitm.empty() || pitm[0] == 0 ? u16(pitm, FULL_HEADER) : u32(pitm, FULL_HEADER);
        }

        bool has_tmap(const std::span<const std::uint8_t> iinf) {
            if (iinf.empty()) {
                return false;
            }

            const std::size_t first = FULL_HEADER + (iinf[0] == 0 ? 2 : 4);

            return std::ranges::any_of(boxes(iinf.subspan(std::min(first, iinf.size()))), [](const Box &entry) {
                if (entry.type != "infe" || entry.body.empty() || entry.body[0] < 2) {
                    return false;
                }

                // The item ID, then a protection index of 16 bits, then the type.
                // Spec: ISO/IEC 14496-12, ItemInfoEntry (infe), versions 2 and 3.
                const std::size_t type = FULL_HEADER + (entry.body[0] == 2 ? 2 : 4) + 2;

                return fourcc(entry.body, type) == "tmap";
            });
        }

        // The 1 based indices into ipco of the properties the item has.
        // Spec: ISO/IEC 23008-12, ItemPropertyAssociationBox (ipma).
        std::vector<std::uint32_t> associated(const std::span<const std::uint8_t> ipma, const std::uint32_t item) {
            std::vector<std::uint32_t> held;

            if (ipma.size() < FULL_HEADER + 4) {
                return held;
            }

            const bool wideIds = ipma[0] >= 1;
            const bool wideIndices = (ipma[3] & 1U) != 0;
            const std::uint32_t entries = u32(ipma, FULL_HEADER);
            std::size_t at = FULL_HEADER + 4;

            for (std::uint32_t e = 0; e < entries && at < ipma.size(); ++e) {
                const std::uint32_t id = wideIds ? u32(ipma, at) : u16(ipma, at);

                at += wideIds ? 4 : 2;

                const std::uint32_t count = at < ipma.size() ? ipma[at] : 0;

                ++at;

                for (std::uint32_t a = 0; a < count && at < ipma.size(); ++a) {
                    const std::uint32_t index = wideIndices ? u16(ipma, at) & 0x7FFFU : ipma[at] & 0x7FU;

                    at += wideIndices ? 2 : 1;

                    if (id == item) {
                        held.push_back(index);
                    }
                }
            }

            return held;
        }
    }

    Heif::Colour Heif::colour(const std::span<const std::uint8_t> data) {
        Colour held;
        const std::span<const std::uint8_t> meta = find(boxes(data), "meta");

        if (meta.size() < FULL_HEADER) {
            return held;
        }

        const std::vector<Box> inside = boxes(meta.subspan(FULL_HEADER));
        const std::vector<Box> properties = boxes(find(inside, "iprp"));
        const std::vector<Box> ipco = boxes(find(properties, "ipco"));

        held.gainMap = has_tmap(find(inside, "iinf")) || std::ranges::any_of(ipco, [](const Box &property) {
                           return property.type == "auxC"
                                  && text(property.body.subspan(std::min(FULL_HEADER, property.body.size())))
                                             == APPLE_GAIN_MAP;
                       });

        const std::uint32_t primary = primary_item(find(inside, "pitm"));

        for (const Box &box : properties) {
            if (box.type != "ipma") {
                continue;
            }

            for (const std::uint32_t index : associated(box.body, primary)) {
                if (index == 0 || index > ipco.size()) {
                    continue;
                }

                const Box &property = ipco.at(index - 1);

                // colour_type, then primaries, transfer and matrix of 16 bits each.
                // Spec: ISO/IEC 14496-12, ColourInformationBox (colr).
                if (property.type == "colr" && fourcc(property.body, 0) == "nclx") {
                    held.tone = Tone::from_cicp(static_cast<int>(u16(property.body, 4)),
                                                static_cast<int>(u16(property.body, 6)));
                }
            }
        }

        return held;
    }

    std::span<const std::uint8_t> Heif::box(const std::span<const std::uint8_t> data, const std::string_view type) {
        return find(boxes(data), type);
    }
}
