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
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <webp/decode.h>

#include "image/Exif.h"
#include "image/Mapped.h"
#include "image/Reorient.h"
#include "image/decode/Decode.h"

namespace tiv {
    namespace {
        using Bytes = std::vector<std::uint8_t>;
        using Exif::Splice;

        constexpr std::string_view EXIF_PREFIX("Exif\0\0", 6);

        struct Plan {
            std::vector<Splice> splices;
            // Why the file cannot take the orientation, when it cannot.
            std::string why;
        };

        bool starts_with(const std::span<const std::uint8_t> data, const std::string_view magic,
                         const std::size_t at = 0) {
            return data.size() >= at + magic.size()
                   && std::equal(magic.begin(), magic.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                                 [](const char a, const std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
        }

        std::uint32_t read_be32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return (static_cast<std::uint32_t>(data[at]) << 24U) | (static_cast<std::uint32_t>(data[at + 1]) << 16U)
                   | (static_cast<std::uint32_t>(data[at + 2]) << 8U) | data[at + 3];
        }

        std::uint32_t read_le32(const std::span<const std::uint8_t> data, const std::size_t at) {
            return data[at] | (static_cast<std::uint32_t>(data[at + 1]) << 8U)
                   | (static_cast<std::uint32_t>(data[at + 2]) << 16U)
                   | (static_cast<std::uint32_t>(data[at + 3]) << 24U);
        }

        void put(Bytes &out, const std::string_view text) {
            out.insert(out.end(), text.begin(), text.end());
        }

        void put(Bytes &out, const std::span<const std::uint8_t> bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        void put_be(Bytes &out, const std::uint32_t value, const int bytes) {
            for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
                out.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }
        }

        void put_le(Bytes &out, const std::uint32_t value, const int bytes) {
            for (int shift = 0; shift < bytes * 8; shift += 8) {
                out.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }
        }

        // The edits a metadata block needs, and whether what it becomes is the smallest block
        // saying 1, which the file did not have before this wrote it.
        struct Edited {
            bool changed = false;
            bool removable = false;
            Bytes block;
        };

        bool edit(const std::span<const std::uint8_t> block, const int orientation, Edited *out) {
            std::vector<Splice> splices;

            if (!Exif::orient(block, orientation, &splices)) {
                return false;
            }

            out->changed = !splices.empty();
            out->block = Exif::apply(block, splices);
            out->removable = orientation == 1 && out->block == Exif::minimal(1);

            return true;
        }

        // --- JPEG: the Exif APP1 segment, https://www.cipa.jp/std/documents/e/DC-X008-Translation-2019-E.pdf, 4.5.4 ---

        Bytes jpeg_segment(const std::span<const std::uint8_t> block) {
            Bytes held = {0xFF, 0xE1};

            put_be(held, static_cast<std::uint32_t>(block.size() + 2 + EXIF_PREFIX.size()), 2);
            put(held, EXIF_PREFIX);
            put(held, block);

            return held;
        }

        Plan plan_jpeg(const std::span<const std::uint8_t> data, const int orientation) {
            constexpr std::uint8_t APP0 = 0xE0;
            constexpr std::uint8_t APP1 = 0xE1;
            constexpr std::uint8_t SOS = 0xDA;
            constexpr std::uint8_t EOI = 0xD9;
            constexpr std::size_t SEGMENT_MAX = 0xFFFF;

            struct Segment {
                std::size_t at = 0;
                std::size_t total = 0;
            };

            Plan plan;
            std::optional<Segment> exif;
            // A new segment goes after the JFIF APP0, which has to come first.
            std::size_t insert = 2;
            bool leading = true;

            for (std::size_t at = 2; at + 4 <= data.size();) {
                if (data[at] != 0xFF) {
                    plan.why = "malformed JPEG";

                    return plan;
                }

                const std::uint8_t marker = data[at + 1];

                if (marker == 0xFF) {
                    ++at;

                    continue;
                }

                if (marker == SOS || marker == EOI) {
                    break;
                }

                const std::size_t total = 2 + ((static_cast<std::size_t>(data[at + 2]) << 8U) | data[at + 3]);

                if (total < 4 || at + total > data.size()) {
                    plan.why = "malformed JPEG";

                    return plan;
                }

                leading = leading && marker == APP0;

                if (leading) {
                    insert = at + total;
                }

                if (marker == APP1 && !exif && starts_with(data, EXIF_PREFIX, at + 4)) {
                    exif = Segment{.at = at, .total = total};
                }

                at += total;
            }

            if (!exif) {
                if (orientation != 1) {
                    plan.splices.push_back(
                            {.at = insert, .length = 0, .bytes = jpeg_segment(Exif::minimal(orientation))});
                }

                return plan;
            }

            const std::size_t header = 4 + EXIF_PREFIX.size();
            Edited edited;

            if (!edit(data.subspan(exif->at + header, exif->total - header), orientation, &edited)) {
                plan.why = "unreadable Exif";
            } else if (edited.removable) {
                plan.splices.push_back({.at = exif->at, .length = exif->total, .bytes = {}});
            } else if (edited.block.size() + header - 2 > SEGMENT_MAX) {
                plan.why = "Exif too large for its segment";
            } else if (edited.changed) {
                plan.splices.push_back({.at = exif->at, .length = exif->total, .bytes = jpeg_segment(edited.block)});
            }

            return plan;
        }

        // --- PNG: the eXIf chunk, https://www.w3.org/TR/png-3/#eXIf ---

        // https://www.w3.org/TR/png-3/#D-CRCAppendix
        std::uint32_t crc32(const std::span<const std::uint8_t> data) {
            static const std::array<std::uint32_t, 256> TABLE = [] {
                std::array<std::uint32_t, 256> held{};

                for (std::uint32_t n = 0; n < held.size(); ++n) {
                    std::uint32_t c = n;

                    for (int k = 0; k < 8; ++k) {
                        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
                    }

                    held.at(n) = c;
                }

                return held;
            }();

            std::uint32_t c = 0xFFFFFFFFU;

            for (const std::uint8_t byte : data) {
                c = TABLE.at((c ^ byte) & 0xFFU) ^ (c >> 8U);
            }

            return c ^ 0xFFFFFFFFU;
        }

        Bytes png_chunk(const std::span<const std::uint8_t> block) {
            Bytes held;

            put_be(held, static_cast<std::uint32_t>(block.size()), 4);
            put(held, "eXIf");
            put(held, block);
            put_be(held, crc32(std::span(held).subspan(4)), 4);

            return held;
        }

        Plan plan_png(const std::span<const std::uint8_t> data, const int orientation) {
            constexpr std::size_t SIGNATURE = 8;
            constexpr std::size_t FRAMING = 12;

            Plan plan;
            std::size_t afterHeader = 0;

            for (std::size_t at = SIGNATURE; at + FRAMING <= data.size();) {
                const std::size_t length = read_be32(data, at);
                const std::size_t total = FRAMING + length;

                if (at + total > data.size() || (at == SIGNATURE && !starts_with(data, "IHDR", at + 4))) {
                    break;
                }

                if (at == SIGNATURE) {
                    afterHeader = at + total;
                }

                if (starts_with(data, "eXIf", at + 4)) {
                    Edited edited;

                    if (!edit(data.subspan(at + 8, length), orientation, &edited)) {
                        plan.why = "unreadable eXIf";
                    } else if (edited.removable) {
                        plan.splices.push_back({.at = at, .length = total, .bytes = {}});
                    } else if (edited.changed) {
                        plan.splices.push_back({.at = at, .length = total, .bytes = png_chunk(edited.block)});
                    }

                    return plan;
                }

                if (starts_with(data, "IEND", at + 4)) {
                    break;
                }

                at += total;
            }

            if (afterHeader == 0) {
                plan.why = "malformed PNG";
            } else if (orientation != 1) {
                plan.splices.push_back(
                        {.at = afterHeader, .length = 0, .bytes = png_chunk(Exif::minimal(orientation))});
            }

            return plan;
        }

        // --- WebP: the EXIF chunk of the extended format, https://developers.google.com/speed/webp/docs/riff_container ---

        constexpr std::size_t RIFF_HEADER = 12;
        constexpr std::size_t CHUNK_HEADER = 8;
        constexpr std::uint8_t FLAG_EXIF = 0x08;
        constexpr std::uint8_t FLAG_ALPHA = 0x10;
        constexpr std::size_t FLAGS_AT = RIFF_HEADER + CHUNK_HEADER;

        struct Chunk {
            std::string_view fourcc;
            std::size_t at = 0;
            std::size_t size = 0;
            std::size_t total = 0;
        };

        Bytes webp_chunk(const std::string_view fourcc, const std::span<const std::uint8_t> payload) {
            Bytes held;

            put(held, fourcc);
            put_le(held, static_cast<std::uint32_t>(payload.size()), 4);
            put(held, payload);

            if ((payload.size() & 1U) != 0) {
                held.push_back(0);
            }

            return held;
        }

        // The header a lone VP8 or VP8L chunk gets, so the file can carry EXIF beside it.
        Bytes webp_extended(const std::span<const std::uint8_t> image) {
            WebPBitstreamFeatures features;

            if (WebPGetFeatures(image.data(), image.size(), &features) != VP8_STATUS_OK) {
                return {};
            }

            Bytes payload = {
                    static_cast<std::uint8_t>(FLAG_EXIF | (features.has_alpha != 0 ? FLAG_ALPHA : std::uint8_t{0})),
                    0,
                    0,
                    0,
            };

            put_le(payload, static_cast<std::uint32_t>(features.width - 1), 3);
            put_le(payload, static_cast<std::uint32_t>(features.height - 1), 3);

            return webp_chunk("VP8X", payload);
        }

        std::vector<Chunk> webp_chunks(const std::span<const std::uint8_t> data) {
            std::vector<Chunk> chunks;

            for (std::size_t at = RIFF_HEADER; at + CHUNK_HEADER <= data.size();) {
                const std::size_t size = read_le32(data, at + 4);

                if (at + CHUNK_HEADER + size > data.size()) {
                    break;
                }

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a fourcc is four characters.
                const std::string_view fourcc{reinterpret_cast<const char *>(data.subspan(at, 4).data()), 4};
                chunks.push_back({
                        .fourcc = fourcc,
                        .at = at,
                        .size = size,
                        .total = CHUNK_HEADER + size + (size & 1U),
                });
                at += chunks.back().total;
            }

            return chunks;
        }

        // A lone image chunk becomes extended, with the EXIF after it.
        void webp_extend(const std::span<const std::uint8_t> data, const int orientation, Plan *plan) {
            const Bytes extended = webp_extended(data.subspan(RIFF_HEADER));

            if (extended.empty()) {
                plan->why = "malformed WebP";

                return;
            }

            plan->splices.push_back({.at = RIFF_HEADER, .length = 0, .bytes = extended});
            plan->splices.push_back(
                    {.at = data.size(), .length = 0, .bytes = webp_chunk("EXIF", Exif::minimal(orientation))});
        }

        // EXIF goes before XMP, the last chunk the container orders.
        void webp_add(const std::span<const std::uint8_t> data, const std::vector<Chunk> &chunks, const int orientation,
                      Plan *plan) {
            const auto xmp = std::ranges::find(chunks, "XMP ", &Chunk::fourcc);

            plan->splices.push_back(
                    {.at = FLAGS_AT, .length = 1, .bytes = {static_cast<std::uint8_t>(data[FLAGS_AT] | FLAG_EXIF)}});
            plan->splices.push_back({
                    .at = xmp != chunks.end() ? xmp->at : data.size(),
                    .length = 0,
                    .bytes = webp_chunk("EXIF", Exif::minimal(orientation)),
            });
        }

        // The EXIF chunk edited, or taken out again with the header webp_extend() added.
        void webp_edit(const std::span<const std::uint8_t> data, const std::vector<Chunk> &chunks, const Chunk &exif,
                       const int orientation, Plan *plan) {
            // Some writers keep the JPEG prefix in the chunk.
            const std::span<const std::uint8_t> payload = data.subspan(exif.at + CHUNK_HEADER, exif.size);
            const std::size_t prefix = starts_with(payload, EXIF_PREFIX) ? EXIF_PREFIX.size() : 0;
            Edited edited;

            if (!edit(payload.subspan(prefix), orientation, &edited)) {
                plan->why = "unreadable EXIF";

                return;
            }

            if (edited.removable && prefix == 0) {
                const std::span<const std::uint8_t> header = data.subspan(RIFF_HEADER, chunks.front().total);
                const bool added =
                        chunks.size() == 3 && exif.at == chunks.at(2).at
                        && std::ranges::equal(header, webp_extended(data.subspan(chunks.at(1).at, chunks.at(1).total)));

                if (added) {
                    plan->splices.push_back({.at = RIFF_HEADER, .length = chunks.front().total, .bytes = {}});
                } else {
                    plan->splices.push_back({
                            .at = FLAGS_AT,
                            .length = 1,
                            .bytes = {static_cast<std::uint8_t>(data[FLAGS_AT] & ~static_cast<unsigned>(FLAG_EXIF))},
                    });
                }

                plan->splices.push_back({.at = exif.at, .length = exif.total, .bytes = {}});
            } else if (edited.changed) {
                Bytes rewritten(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(prefix));

                put(rewritten, edited.block);
                plan->splices.push_back({.at = exif.at, .length = exif.total, .bytes = webp_chunk("EXIF", rewritten)});
            }
        }

        Plan plan_webp(const std::span<const std::uint8_t> data, const int orientation) {
            Plan plan;
            const std::vector<Chunk> chunks = webp_chunks(data);

            if (chunks.empty()) {
                plan.why = "malformed WebP";

                return plan;
            }

            const auto exif = std::ranges::find(chunks, "EXIF", &Chunk::fourcc);

            if (exif != chunks.end()) {
                webp_edit(data, chunks, *exif, orientation, &plan);
            } else if (orientation == 1) {
                return plan;
            } else if (chunks.front().fourcc == "VP8X") {
                webp_add(data, chunks, orientation, &plan);
            } else {
                webp_extend(data, orientation, &plan);
            }

            if (!plan.why.empty() || plan.splices.empty()) {
                return plan;
            }

            std::size_t size = data.size();

            for (const Splice &splice : plan.splices) {
                size = size + splice.bytes.size() - splice.length;
            }

            Bytes riff;

            put_le(riff, static_cast<std::uint32_t>(size - CHUNK_HEADER), 4);
            plan.splices.insert(plan.splices.begin(), {.at = 4, .length = 4, .bytes = riff});

            return plan;
        }

        // --- TIFF: the Orientation tag of IFD0 ---

        Plan plan_tiff(const std::span<const std::uint8_t> data, const int orientation) {
            Plan plan;

            if (!Exif::orient(data, orientation, &plan.splices)) {
                plan.why = "unsupported TIFF layout";
            }

            return plan;
        }

        void fail(std::string *error, const std::filesystem::path &file, const std::string_view why) {
            if (error != nullptr) {
                *error = file.string() + ": " + std::string(why);
            }
        }

        // Splices of the same length as what they replace are written in place, which keeps
        // the file's identity and links. Anything else is written beside it and renamed over.
        bool commit(const std::filesystem::path &file, Mapped mapped, const std::vector<Splice> &splices,
                    std::string *error) {
            if (splices.empty()) {
                return true;
            }

            if (std::ranges::all_of(splices,
                                    [](const Splice &splice) { return splice.bytes.size() == splice.length; })) {
                mapped = {};

                std::fstream out(file, std::ios::in | std::ios::out | std::ios::binary);

                for (const Splice &splice : splices) {
                    out.seekp(static_cast<std::streamoff>(splice.at));
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streams write chars.
                    out.write(reinterpret_cast<const char *>(splice.bytes.data()),
                              static_cast<std::streamsize>(splice.bytes.size()));
                }

                out.flush();

                if (!out) {
                    fail(error, file, "could not write");

                    return false;
                }

                return true;
            }

            const std::filesystem::path temporary = file.parent_path() / ("." + file.filename().string() + ".tinyiv");
            const std::span<const std::uint8_t> data = mapped.data();
            std::error_code failure;

            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                std::size_t from = 0;

                const auto write = [&](const std::span<const std::uint8_t> bytes) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streams write chars.
                    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                };

                for (const Splice &splice : splices) {
                    write(data.subspan(from, splice.at - from));
                    write(splice.bytes);
                    from = splice.at + splice.length;
                }

                write(data.subspan(from));
                out.close();

                if (!out) {
                    std::filesystem::remove(temporary, failure);
                    fail(error, file, "could not write");

                    return false;
                }
            }

            std::filesystem::permissions(temporary, std::filesystem::status(file).permissions(), failure);
            mapped = {};
            std::filesystem::rename(temporary, file, failure);

            if (failure) {
                std::filesystem::remove(temporary, failure);
                fail(error, file, failure.message());

                return false;
            }

            return true;
        }
    }

    bool Reorient::supported(const Decode::Format format) {
        switch (format) {
            case Decode::Format::Jpeg:
            case Decode::Format::Png:
            case Decode::Format::WebP:
            case Decode::Format::Tiff:
                return true;
            case Decode::Format::Jxl:
            case Decode::Format::Gif:
            case Decode::Format::Bmp:
            case Decode::Format::Ico:
            case Decode::Format::Icns:
            case Decode::Format::Heif:
            case Decode::Format::Svg:
            case Decode::Format::Pdf:
            case Decode::Format::Other:
                break;
        }

        return false;
    }

    bool Reorient::write(const std::filesystem::path &file, const int orientation, std::string *error) {
        std::error_code failure;
        // A link stays a link, and what it points at changes.
        const std::filesystem::path target =
                std::filesystem::is_symlink(file, failure) ? std::filesystem::canonical(file, failure) : file;
        Mapped mapped;

        if (failure) {
            fail(error, file, failure.message());

            return false;
        }

        if (!Mapped::open(target, &mapped, error)) {
            return false;
        }

        Plan plan;

        switch (Decode::sniff(mapped.data())) {
            case Decode::Format::Jpeg:
                plan = plan_jpeg(mapped.data(), orientation);
                break;
            case Decode::Format::Png:
                plan = plan_png(mapped.data(), orientation);
                break;
            case Decode::Format::WebP:
                plan = plan_webp(mapped.data(), orientation);
                break;
            case Decode::Format::Tiff:
                plan = plan_tiff(mapped.data(), orientation);
                break;
            default:
                plan.why = "this format keeps no orientation";
                break;
        }

        if (!plan.why.empty()) {
            fail(error, file, plan.why);

            return false;
        }

        return commit(target, std::move(mapped), plan.splices, error);
    }
}
