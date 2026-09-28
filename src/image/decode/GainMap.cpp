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
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "image/Bitmap.h"
#include "image/decode/GainMap.h"

namespace tiv {
    namespace {
        constexpr std::string_view XMP_SIGNATURE{"http://ns.adobe.com/xap/1.0/\0", 29};
        constexpr std::string_view ISO_SIGNATURE{"urn:iso:std:iso:ts:21496:-1\0", 28};
        constexpr std::string_view MPF_SIGNATURE{"MPF\0", 4};

        constexpr std::uint8_t MARKER_APP1 = 0xE1;
        constexpr std::uint8_t MARKER_APP2 = 0xE2;
        constexpr std::uint8_t MARKER_SOS = 0xDA;
        constexpr std::uint16_t TAG_MP_ENTRY = 0xB002;
        constexpr std::size_t MP_ENTRY_BYTES = 16;

        constexpr std::uint8_t MULTICHANNEL = 0x80;
        constexpr std::uint8_t BASE_COLOURS = 0x40;

        struct Reader {
            std::span<const std::uint8_t> data;
            bool little = false;

            [[nodiscard]] std::uint32_t u16(const std::size_t at) const {
                if (at + 2 > data.size()) {
                    return 0;
                }

                return little ? static_cast<std::uint32_t>(data[at] | (data[at + 1] << 8)) : static_cast<std::uint32_t>((data[at] << 8) | data[at + 1]);
            }

            [[nodiscard]] std::uint32_t u32(const std::size_t at) const {
                return little ? u16(at) | (u16(at + 2) << 16) : (u16(at) << 16) | u16(at + 2);
            }
        };

        bool starts_with(const std::span<const std::uint8_t> data, const std::string_view prefix) {
            return data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin(), [](const char a, const std::uint8_t b) {
                return static_cast<std::uint8_t>(a) == b;
            });
        }

        struct Segment {
            std::uint8_t marker = 0;
            std::span<const std::uint8_t> body;
        };

        // The marker segments of a JPEG up to the start of its scan: ITU-T T.81, B.1.
        std::vector<Segment> segments(const std::span<const std::uint8_t> data) {
            std::vector<Segment> held;

            if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
                return held;
            }

            for (std::size_t at = 2; at + 4 <= data.size() && data[at] == 0xFF;) {
                const std::uint8_t marker = data[at + 1];

                if (marker == 0xFF) {
                    ++at;

                    continue;
                }

                if (marker == MARKER_SOS) {
                    break;
                }

                const std::size_t length = (static_cast<std::size_t>(data[at + 2]) << 8) | data[at + 3];

                if (length < 2 || at + 2 + length > data.size()) {
                    break;
                }

                held.push_back({marker, data.subspan(at + 4, length - 2)});
                at += 2 + length;
            }

            return held;
        }

        // The value of an XMP property, written as an attribute or as an element.
        std::string_view property(const std::string_view xmp, const std::string_view name) {
            const std::string_view prefix = "hdrgm:";

            for (std::size_t at = xmp.find(prefix); at != std::string_view::npos; at = xmp.find(prefix, at + 1)) {
                const std::size_t after = at + prefix.size() + name.size();

                if (xmp.substr(at + prefix.size(), name.size()) != name || after >= xmp.size()) {
                    continue;
                }

                if (xmp.at(after) == '=' && after + 1 < xmp.size()) {
                    const char quote = xmp.at(after + 1);
                    const std::size_t end = xmp.find(quote, after + 2);

                    if (end != std::string_view::npos) {
                        return xmp.substr(after + 2, end - after - 2);
                    }
                } else if (xmp.at(after) == '>' && at > 0 && xmp.at(at - 1) == '<') {
                    // An rdf:Seq holds nested closing tags, so the element's own is looked for.
                    const std::size_t close = xmp.find(std::string("</hdrgm:").append(name), after + 1);

                    if (close != std::string_view::npos) {
                        return xmp.substr(after + 1, close - after - 1);
                    }
                }
            }

            return {};
        }

        std::optional<float> number(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\t' || text.front() == '+')) {
                text.remove_prefix(1);
            }

            float value = 0.0F;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars takes a pointer range.
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

            if (error != std::errc{} || end == text.data()) {
                return std::nullopt;
            }

            return value;
        }

        // One value, or one per channel inside rdf:li elements.
        std::vector<float> numbers(const std::string_view text) {
            constexpr std::string_view ITEM = "<rdf:li>";
            std::vector<float> held;

            for (std::size_t at = text.find(ITEM); at != std::string_view::npos; at = text.find(ITEM, at + 1)) {
                if (const std::optional<float> value = number(text.substr(at + ITEM.size()))) {
                    held.push_back(*value);
                }
            }

            if (held.empty()) {
                if (const std::optional<float> value = number(text)) {
                    held.push_back(*value);
                }
            }

            return held;
        }

        // Fills all three channels, from one value or three. False when the property is
        // there but unreadable, or missing where it has no default.
        bool channels(const std::string_view xmp, const std::string_view name, std::array<float, 3> *out, int *count, const bool required) {
            const std::string_view text = property(xmp, name);

            if (text.empty()) {
                return !required;
            }

            const std::vector<float> values = numbers(text);

            if (values.size() != 1 && values.size() != 3) {
                return false;
            }

            for (std::size_t c = 0; c < 3; ++c) {
                out->at(c) = values.at(values.size() == 1 ? 0 : c);
            }

            *count = std::max(*count, static_cast<int>(values.size()));

            return true;
        }

        bool single(const std::string_view xmp, const std::string_view name, float *out, const bool required) {
            const std::string_view text = property(xmp, name);

            if (text.empty()) {
                return !required;
            }

            const std::optional<float> value = number(text);

            if (value) {
                *out = *value;
            }

            return value.has_value();
        }

        std::string_view as_text(const std::span<const std::uint8_t> data) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): XMP is UTF-8.
            return {reinterpret_cast<const char *>(data.data()), data.size()};
        }

        // The metadata a gain map image carries in its own markers, the binary form first.
        bool metadata_of(const std::span<const std::uint8_t> image, GainMap::Metadata *out) {
            const std::vector<Segment> found = segments(image);

            for (const Segment &segment : found) {
                if (segment.marker == MARKER_APP2 && starts_with(segment.body, ISO_SIGNATURE) && GainMap::parse_iso(segment.body.subspan(ISO_SIGNATURE.size()), out)) {
                    return true;
                }
            }

            return std::ranges::any_of(found, [out](const Segment &segment) {
                return segment.marker == MARKER_APP1 && starts_with(segment.body, XMP_SIGNATURE) && GainMap::parse_xmp(as_text(segment.body.subspan(XMP_SIGNATURE.size())), out);
            });
        }

        // The images after the primary one, from the MP entries in its APP2 segment.
        std::vector<std::span<const std::uint8_t>> mpf_images(const std::span<const std::uint8_t> data) {
            std::vector<std::span<const std::uint8_t>> held;

            for (const Segment &segment : segments(data)) {
                if (segment.marker != MARKER_APP2 || !starts_with(segment.body, MPF_SIGNATURE)) {
                    continue;
                }

                // Offsets count from the TIFF header that follows the signature.
                const std::span<const std::uint8_t> tiff = segment.body.subspan(MPF_SIGNATURE.size());
                const auto origin = static_cast<std::size_t>(tiff.data() - data.data());
                const Reader reader{tiff, starts_with(tiff, "II")};
                const std::size_t ifd = reader.u32(4);
                const std::uint32_t count = reader.u16(ifd);

                for (std::uint32_t e = 0; e < count; ++e) {
                    const std::size_t entry = ifd + 2 + (static_cast<std::size_t>(e) * 12);

                    if (reader.u16(entry) != TAG_MP_ENTRY) {
                        continue;
                    }

                    const std::size_t images = reader.u32(entry + 4) / MP_ENTRY_BYTES;
                    const std::size_t table = reader.u32(entry + 8);

                    // The first entry is the primary image itself.
                    for (std::size_t i = 1; i < images; ++i) {
                        const std::size_t size = reader.u32(table + (i * MP_ENTRY_BYTES) + 4);
                        const std::size_t offset = reader.u32(table + (i * MP_ENTRY_BYTES) + 8);

                        if (offset != 0 && origin + offset < data.size() && size <= data.size() - origin - offset) {
                            held.push_back(data.subspan(origin + offset, size));
                        }
                    }
                }
            }

            return held;
        }

        constexpr std::size_t GAIN_STEPS = 1024;
    }

    float GainMap::Metadata::weight(const float headroom) const {
        if (baseHeadroom == alternateHeadroom) {
            return 0.0F;
        }

        const float w = std::clamp((headroom - baseHeadroom) / (alternateHeadroom - baseHeadroom), 0.0F, 1.0F);

        return alternateHeadroom < baseHeadroom ? -w : w;
    }

    // Fractions of 32 bit integers, signed where a value can be negative. The layout libavif
    // writes: https://github.com/AOMediaCodec/libavif/blob/main/src/read.c
    bool GainMap::parse_iso(const std::span<const std::uint8_t> data, Metadata *out) {
        const auto parse = [&](const std::size_t start) {
            const Reader reader{data, false};

            if (start + 5 > data.size() || reader.u16(start) != 0) {
                return false;
            }

            const std::uint8_t flags = data[start + 4];
            const std::size_t count = (flags & MULTICHANNEL) != 0 ? 3 : 1;

            if (data.size() != start + 5 + 16 + (count * 40)) {
                return false;
            }

            std::size_t at = start + 5;
            bool valid = true;

            const auto fraction = [&](const bool isSigned) {
                const std::uint32_t numerator = reader.u32(at);
                const std::uint32_t denominator = reader.u32(at + 4);

                at += 8;

                if (denominator == 0) {
                    valid = false;

                    return 0.0F;
                }

                const double top = isSigned ? static_cast<double>(static_cast<std::int32_t>(numerator)) : static_cast<double>(numerator);

                return static_cast<float>(top / denominator);
            };

            Metadata held;

            held.channels = static_cast<int>(count);
            held.inBaseColours = (flags & BASE_COLOURS) != 0;
            held.baseHeadroom = fraction(false);
            held.alternateHeadroom = fraction(false);

            for (std::size_t c = 0; c < count; ++c) {
                held.min.at(c) = fraction(true);
                held.max.at(c) = fraction(true);
                held.gamma.at(c) = fraction(false);
                held.baseOffset.at(c) = fraction(true);
                held.alternateOffset.at(c) = fraction(true);
            }

            for (std::size_t c = count; c < 3; ++c) {
                held.min.at(c) = held.min.at(0);
                held.max.at(c) = held.max.at(0);
                held.gamma.at(c) = held.gamma.at(0);
                held.baseOffset.at(c) = held.baseOffset.at(0);
                held.alternateOffset.at(c) = held.alternateOffset.at(0);
            }

            if (!valid || std::ranges::any_of(held.gamma, [](const float g) { return g <= 0.0F; })) {
                return false;
            }

            *out = held;

            return true;
        };

        return parse(0) || (!data.empty() && data[0] == 0 && parse(1));
    }

    bool GainMap::parse_xmp(const std::string_view xmp, Metadata *out) {
        if (property(xmp, "Version").empty() || property(xmp, "BaseRenditionIsHDR") == "True") {
            return false;
        }

        Metadata held;
        int count = 1;
        float capacityMin = 0.0F;
        float capacityMax = 0.0F;

        std::array<float, 3> offsetSdr = held.baseOffset;
        std::array<float, 3> offsetHdr = held.alternateOffset;

        const bool read = channels(xmp, "GainMapMin", &held.min, &count, false) && channels(xmp, "GainMapMax", &held.max, &count, true)
                          && channels(xmp, "Gamma", &held.gamma, &count, false) && channels(xmp, "OffsetSDR", &offsetSdr, &count, false)
                          && channels(xmp, "OffsetHDR", &offsetHdr, &count, false) && single(xmp, "HDRCapacityMin", &capacityMin, false)
                          && single(xmp, "HDRCapacityMax", &capacityMax, true);

        if (!read || std::ranges::any_of(held.gamma, [](const float g) { return g <= 0.0F; })) {
            return false;
        }

        held.channels = count;
        held.baseOffset = offsetSdr;
        held.alternateOffset = offsetHdr;
        held.baseHeadroom = capacityMin;
        held.alternateHeadroom = capacityMax;
        *out = held;

        return true;
    }

    bool GainMap::find_jpeg(const std::span<const std::uint8_t> data, Jpeg *out) {
        return std::ranges::any_of(mpf_images(data), [out](const std::span<const std::uint8_t> image) {
            Metadata metadata;

            if (!metadata_of(image, &metadata)) {
                return false;
            }

            *out = {image, metadata};

            return true;
        });
    }

    bool GainMap::read_jxl(const std::span<const std::uint8_t> box, Jxl *out) {
        const Reader reader{box, false};

        if (box.size() < 3) {
            return false;
        }

        const std::size_t metadataSize = reader.u16(1);
        std::size_t at = 3 + metadataSize;

        if (at >= box.size()) {
            return false;
        }

        Jxl held;

        if (!parse_iso(box.subspan(3, metadataSize), &held.metadata)) {
            return false;
        }

        // The encoding is a bit packed bundle whose first bit, all_default, says sRGB.
        const std::size_t encodingSize = box[at];

        if (encodingSize > 0 && at + 1 < box.size()) {
            held.alternateSrgb = (box[at + 1] & 1U) != 0;
        }

        at += 1 + encodingSize;

        if (at + 4 > box.size()) {
            return false;
        }

        const std::size_t iccSize = reader.u32(at);

        at += 4;

        if (iccSize > box.size() - at || (!held.metadata.inBaseColours && !held.alternateSrgb)) {
            return false;
        }

        held.image = box.subspan(at + iccSize);
        *out = held;

        return !held.image.empty();
    }

    GainMap::Applier::Applier(const Metadata &metadata, const Bitmap *map, const int width, const int height, const float weight)
        : _metadata(metadata), _map(map), _scaleX(static_cast<float>(map->width()) / static_cast<float>(width)),
          _scaleY(static_cast<float>(map->height()) / static_cast<float>(height)) {
        for (std::size_t c = 0; c < 3; ++c) {
            _factors.at(c).resize(GAIN_STEPS);

            for (std::size_t i = 0; i < GAIN_STEPS; ++i) {
                const float g = std::pow(static_cast<float>(i) / static_cast<float>(GAIN_STEPS - 1), 1.0F / metadata.gamma.at(c));
                const float stops = std::lerp(metadata.min.at(c), metadata.max.at(c), g);

                _factors.at(c).at(i) = std::exp2(stops * weight);
            }
        }
    }

    // The map is sampled between its pixels, the way a smaller map is stretched over the image.
    // One channel is sampled once for all three.
    void GainMap::Applier::apply(const int x, const int y, const std::span<float> rgba) const {
        const int mapWidth = _map->width();
        const int mapHeight = _map->height();
        const float fy = std::clamp(((static_cast<float>(y) + 0.5F) * _scaleY) - 0.5F, 0.0F, static_cast<float>(mapHeight - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, mapHeight - 1);
        const float ty = fy - static_cast<float>(y0);
        const std::span<const std::uint8_t> above = _map->row(y0);
        const std::span<const std::uint8_t> below = _map->row(y1);
        const std::size_t bands = _metadata.channels == 1 ? 1 : 3;
        const std::array<float, 3> &base = _metadata.baseOffset;
        const std::array<float, 3> &alternate = _metadata.alternateOffset;
        const std::array<const float *, 3> factors{_factors.at(0).data(), _factors.at(1).data(), _factors.at(2).data()};

        for (std::size_t at = 0; at + 4 <= rgba.size(); at += 4) {
            const std::size_t pixel = at / 4;
            const float fx = std::clamp(((static_cast<float>(x) + static_cast<float>(pixel) + 0.5F) * _scaleX) - 0.5F, 0.0F, static_cast<float>(mapWidth - 1));
            const auto x0 = static_cast<std::size_t>(fx);
            const std::size_t x1 = std::min(x0 + 1, static_cast<std::size_t>(mapWidth - 1));
            const float tx = fx - static_cast<float>(x0);
            std::array<std::size_t, 3> steps{};

            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-incorrect-roundings): the rows hold every x0 and x1, the steps stay within the factors, and nothing is negative.
            for (std::size_t band = 0; band < bands; ++band) {
                const float top = std::lerp(static_cast<float>(above[(x0 * 4) + band]), static_cast<float>(above[(x1 * 4) + band]), tx);
                const float bottom = std::lerp(static_cast<float>(below[(x0 * 4) + band]), static_cast<float>(below[(x1 * 4) + band]), tx);

                steps[band] = static_cast<std::size_t>((std::lerp(top, bottom, ty) * static_cast<float>(GAIN_STEPS - 1) / 255.0F) + 0.5F);
            }

            for (std::size_t c = 0; c < 3; ++c) {
                rgba[at + c] = ((rgba[at + c] + base[c]) * factors[c][steps[bands == 1 ? 0 : c]]) - alternate[c];
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-incorrect-roundings)
        }
    }
}
