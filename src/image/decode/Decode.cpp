// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include <vips/vips.h>

#include "image/Bitmap.h"
#include "image/Mapped.h"
#include "image/Tone.h"
#include "image/decode/Bmp.h"
#include "image/decode/Decode.h"
#include "image/decode/Heif.h"
#include "image/decode/Icon.h"
#include "image/decode/Jpeg.h"
#include "image/decode/Jxl.h"
#include "image/decode/Png.h"
#include "image/decode/Support.h"
#include "image/decode/Vips.h"
#include "image/decode/WebP.h"

namespace tiv {
    namespace {
        constexpr std::array<std::string_view, 34> SUFFIXES = {
                ".jpg",  ".jpeg", ".jpe",  ".jfif", ".png", ".webp", ".jxl", ".gif", ".bmp", ".ico",  ".icns", ".tif",
                ".tiff", ".heic", ".heif", ".avif", ".svg", ".svgz", ".pdf", ".jp2", ".j2k", ".jpx",  ".exr",  ".hdr",
                ".ppm",  ".pgm",  ".pbm",  ".pnm",  ".pfm", ".fits", ".fit", ".nii", ".v",   ".vips",
        };

        std::string_view format_name(const Decode::Format kind) {
            switch (kind) {
                case Decode::Format::Jpeg:
                    return "JPEG";
                case Decode::Format::Png:
                    return "PNG";
                case Decode::Format::WebP:
                    return "WEBP";
                case Decode::Format::Jxl:
                    return "JXL";
                case Decode::Format::Gif:
                    return "GIF";
                case Decode::Format::Bmp:
                    return "BMP";
                case Decode::Format::Ico:
                    return "ICO";
                case Decode::Format::Icns:
                    return "ICNS";
                case Decode::Format::Tiff:
                    return "TIFF";
                case Decode::Format::Heif:
                    return "HEIF";
                case Decode::Format::Svg:
                    return "SVG";
                case Decode::Format::Pdf:
                    return "PDF";
                case Decode::Format::Other:
                    break;
            }

            return {};
        }

        bool probe_bmp(const std::span<const std::uint8_t> data, Decode::Info *info) {
            Bmp::Image image;

            if (!Bmp::Image::open(data, &image)) {
                return false;
            }

            info->width = image.width();
            info->height = image.height();

            return true;
        }

        // A variant the decoder does not know, such as an embedded JPEG, is left to libvips.
        Decode::Direct load_bmp(const std::filesystem::path &file, const std::span<const std::uint8_t> data,
                                const int boxWidth, const int boxHeight, Bitmap *out, std::string *error,
                                const Decode::Abort *abort, const Decode::Fit fit) {
            Bmp::Image image;

            if (!Bmp::Image::open(data, &image)) {
                return Decode::Direct::Skip;
            }

            const int factor = fit == Decode::Fit::Force
                                       ? Decode::shrink_factor(image.width(), image.height(), boxWidth, boxHeight)
                                       : 1;

            if (!image.decode(factor, out, abort)) {
                Decode::fail(error, file, Decode::aborted(abort) ? "aborted" : "bmp decode failed");

                return Decode::Direct::Failed;
            }

            return Decode::Direct::Done;
        }

        bool icon_entry(const Decode::Format kind, const std::span<const std::uint8_t> data, Icon::Entry *entry) {
            return kind == Decode::Format::Ico ? Icon::largest_ico(data, entry) : Icon::largest_icns(data, entry);
        }

        bool probe_icon(const Decode::Format kind, const std::span<const std::uint8_t> data, Decode::Info *info) {
            Icon::Entry entry;

            if (!icon_entry(kind, data, &entry)) {
                return false;
            }

            info->width = entry.width;
            info->height = entry.height;

            return true;
        }

        // Only the largest entry is shown.
        Decode::Direct load_icon(const std::filesystem::path &file, const Decode::Format kind,
                                 const std::span<const std::uint8_t> data, const int boxWidth, const int boxHeight,
                                 Bitmap *out, std::string *error, Decode::Abort *abort, const Decode::Fit fit) {
            Icon::Entry entry;

            if (!icon_entry(kind, data, &entry)) {
                return Decode::Direct::Skip;
            }

            switch (entry.payload) {
                case Icon::Payload::Png:
                    return Decode::Png::load(file, entry.data, boxWidth, boxHeight, out, error, abort, fit);
                case Icon::Payload::Jpeg2000:
                    return Decode::Vips::load_buffer(file, entry.data, out, error, abort) ? Decode::Direct::Done
                                                                                          : Decode::Direct::Failed;
                case Icon::Payload::Packed:
                    if (!Icon::unpack(entry, out)) {
                        Decode::fail(error, file, "icns decode failed");

                        return Decode::Direct::Failed;
                    }

                    return Decode::Direct::Done;
                case Icon::Payload::Dib: {
                    Bmp::Image image;

                    if (!Bmp::Image::open_icon(entry.data, &image)) {
                        Decode::fail(error, file, "ico entry unreadable");

                        return Decode::Direct::Failed;
                    }

                    const int factor = fit == Decode::Fit::Force ? Decode::shrink_factor(image.width(), image.height(),
                                                                                         boxWidth, boxHeight)
                                                                 : 1;

                    if (!image.decode(factor, out, abort)) {
                        Decode::fail(error, file, Decode::aborted(abort) ? "aborted" : "ico decode failed");

                        return Decode::Direct::Failed;
                    }

                    return Decode::Direct::Done;
                }
            }

            return Decode::Direct::Skip;
        }
    }

    void Decode::Abort::request() {
        const std::scoped_lock hold(_guard);

        _requested.store(true, std::memory_order_relaxed);

        if (_image != nullptr) {
            vips_image_set_kill(_image, TRUE);
        }
    }

    void Decode::Abort::reset() {
        const std::scoped_lock hold(_guard);

        _requested.store(false, std::memory_order_relaxed);
    }

    void Decode::Abort::arm(_VipsImage *image) {
        const std::scoped_lock hold(_guard);

        _image = image;

        if (_requested.load(std::memory_order_relaxed)) {
            vips_image_set_kill(_image, TRUE);
        }
    }

    void Decode::Abort::disarm() {
        const std::scoped_lock hold(_guard);

        _image = nullptr;
    }

    std::span<const std::string_view> Decode::suffixes() {
        return SUFFIXES;
    }

    Decode::Format Decode::sniff(const std::span<const std::uint8_t> head) {
        if (starts_with(head, "\xFF\xD8\xFF")) {
            return Format::Jpeg;
        }

        if (starts_with(head, "\x89PNG\r\n\x1A\n")) {
            return Format::Png;
        }

        if (starts_with(head, "RIFF") && starts_with(head, "WEBP", 8)) {
            return Format::WebP;
        }

        if (starts_with(head, "\xFF\x0A") || starts_with(head, std::string_view("\0\0\0\x0CJXL \r\n\x87\n", 12))) {
            return Format::Jxl;
        }

        if (starts_with(head, "GIF8")) {
            return Format::Gif;
        }

        if (starts_with(head, "BM")) {
            return Format::Bmp;
        }

        if (starts_with(head, std::string_view("\0\0\1\0", 4)) && head.size() >= 6 && (head[4] != 0 || head[5] != 0)) {
            return Format::Ico;
        }

        if (starts_with(head, "icns")) {
            return Format::Icns;
        }

        if (starts_with(head, "II*\0") || starts_with(head, "MM\0*")) {
            return Format::Tiff;
        }

        if (starts_with(head, "ftyp", 4)) {
            return Format::Heif;
        }

        if (starts_with(head, "%PDF")) {
            return Format::Pdf;
        }

        if (starts_with(head, "<?xml") || starts_with(head, "<svg")) {
            return Format::Svg;
        }

        return Format::Other;
    }

    bool Decode::scales_cheaply(const Format format) {
        switch (format) {
            case Format::Jpeg:
            case Format::Svg:
            case Format::Pdf:
                return true;
            case Format::Png:
            case Format::WebP:
            case Format::Jxl:
            case Format::Gif:
            case Format::Bmp:
            case Format::Ico:
            case Format::Icns:
            case Format::Tiff:
            case Format::Heif:
            case Format::Other:
                break;
        }

        return false;
    }

    bool Decode::scalable(const Format format) {
        return format == Format::Svg || format == Format::Pdf;
    }

    bool Decode::recognised(const std::filesystem::path &file) {
        Mapped mapped;

        if (Mapped::open(file, &mapped) && sniff(mapped.data()) != Format::Other) {
            return true;
        }

        Vips::ensure();

        const bool known = vips_foreign_find_load(file.string().c_str()) != nullptr;

        vips_error_clear();

        return known;
    }

    bool Decode::probe(const std::filesystem::path &file, Info *info, std::string *error) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped, error)) {
            return false;
        }

        *info = {};
        info->kind = sniff(mapped.data());
        info->format = format_name(info->kind);

        bool known = false;

        switch (info->kind) {
            case Format::Jpeg:
                known = Jpeg::probe(mapped.data(), info);
                break;
            case Format::Png:
                known = Png::probe(mapped.data(), info);
                break;
            case Format::WebP:
                known = WebP::probe(mapped.data(), info);
                break;
            case Format::Jxl:
                known = Jxl::probe(mapped.data(), info);
                break;
            case Format::Bmp:
                known = probe_bmp(mapped.data(), info);
                break;
            case Format::Ico:
            case Format::Icns:
                known = probe_icon(info->kind, mapped.data(), info);
                break;
            default:
                break;
        }

        if (known) {
            return true;
        }

        if (info->kind == Format::Heif) {
            const Heif::Colour colour = Heif::colour(mapped.data());

            info->hdr = colour.tone.hdr() || colour.gainMap;
        } else {
            info->hdr = Vips::container_tone(info->kind, mapped.data()).hdr();
        }

        mapped = {};

        return Vips::probe(file, info, error);
    }

    bool Decode::load(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out,
                      std::string *error, Abort *abort, const Fit fit, const Tone::Display &display) {
        Mapped mapped;

        if (!Mapped::open(file, &mapped, error)) {
            return false;
        }

        const Format kind = sniff(mapped.data());
        Direct direct = Direct::Skip;

        switch (kind) {
            case Format::Jpeg:
                direct = Jpeg::load(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit, display);
                break;
            case Format::Png:
                direct = Png::load(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit, display);
                break;
            case Format::WebP:
                direct = fit == Fit::Cheap ? WebP::load(file, mapped.data(), out, error, abort) : Direct::Skip;
                break;
            case Format::Jxl:
                direct = fit == Fit::Cheap ? Jxl::load(file, mapped.data(), out, error, abort, display) : Direct::Skip;
                break;
            case Format::Bmp:
                direct = load_bmp(file, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            case Format::Ico:
            case Format::Icns:
                direct = load_icon(file, kind, mapped.data(), boxWidth, boxHeight, out, error, abort, fit);
                break;
            default:
                break;
        }

        if (direct != Direct::Skip) {
            return direct == Direct::Done;
        }

        const Tone::Source tone = Vips::container_tone(kind, mapped.data());

        mapped = {};

        if (aborted(abort)) {
            fail(error, file, "aborted");

            return false;
        }

        Vips::Via via = Vips::Via::Whole;

        if (scalable(kind)) {
            via = Vips::Via::Scaled;
        } else if (scales_cheaply(kind)) {
            via = Vips::Via::Thumbnail;
        } else if (fit == Fit::Force) {
            via = Vips::Via::Shrink;
        }

        return Vips::load(file, boxWidth, boxHeight, out, error, abort, via, tone, display);
    }

    bool Decode::load_png_memory(const std::span<const std::uint8_t> data, Bitmap *out, std::string *error) {
        constexpr int WHOLE = std::numeric_limits<int>::max();

        return Png::load("memory", data, WHOLE, WHOLE, out, error, nullptr, Fit::Cheap) == Direct::Done;
    }

    void Decode::shutdown() {
        Vips::shutdown();
    }
}
