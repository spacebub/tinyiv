// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_VIPS_H
#define TIV_DECODE_VIPS_H


#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <vips/vips8>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"

namespace tiv::Decode::Vips {

    void ensure();

    // What libvips last reported, which is then cleared.
    [[nodiscard]] std::string last_error();

    void shutdown();

    // An image on its way to RGBA8: converted by libvips, or, when HDR, left in wide
    // samples of 1 to 4 bands for the tone mapper.
    struct Prepared {
        vips::VImage image;
        std::optional<Tone::Mapper> mapper;
        Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
    };

    // Float samples are linear light whatever the file says. Wider integers carry their
    // transfer in the container, which the caller has read.
    [[nodiscard]] Prepared prepare(vips::VImage image, Tone::Source source, const Tone::Display &display = {});

    // Count rows from y into RGBA8 at target, packed.
    bool write_rows(const Prepared &prepared, int y, int count, std::uint8_t *target, Abort *abort);

    bool write_rgba(const Prepared &prepared, Bitmap *out, Abort *abort);

    bool probe(const std::filesystem::path &file, Info *info, std::string *error);

    enum class Via : std::uint8_t {
        // The image as it is.
        Whole,
        // Rendered or shrunk on load to the box, for formats that do that cheaply.
        Thumbnail,
        // Rendered to fit the box, larger or smaller, for formats drawn from shapes.
        Scaled,
        // Streamed through an integer box shrink, so memory is the output and not the input.
        Shrink,
    };

    bool load(const std::filesystem::path &file, int boxWidth, int boxHeight, Bitmap *out, std::string *error, Abort *abort, Via via, Tone::Source source, const Tone::Display &display);

    // The data has to stay mapped until the image is written.
    bool load_buffer(const std::filesystem::path &file, std::span<const std::uint8_t> data, Bitmap *out, std::string *error, Abort *abort);

    // The transfer of an HDR file that libvips reads without it.
    [[nodiscard]] Tone::Source container_tone(Format kind, std::span<const std::uint8_t> data);
    [[nodiscard]] Tone::Source container_tone(const std::filesystem::path &file);

}


#endif //TIV_DECODE_VIPS_H
