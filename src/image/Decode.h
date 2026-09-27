// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_DECODE_H
#define TIV_IMAGE_DECODE_H


#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "image/Bitmap.h"

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the tag libvips gives VipsImage.
struct _VipsImage;

namespace tiv::Decode {

    enum class Format : std::uint8_t {
        Jpeg,
        Png,
        WebP,
        Jxl,
        Gif,
        Bmp,
        Ico,
        Icns,
        Tiff,
        Heif,
        Svg,
        Pdf,
        // Whatever libvips makes of it.
        Other,
    };

    struct Info {
        // As displayed, so orientation already applied.
        int width = 0;
        int height = 0;
        // Short upper case name of the container: JPEG, PNG, WEBP.
        std::string format;
        Format kind = Format::Other;
        // EXIF orientation, 1 to 8.
        int orientation = 1;
        // More than one for an animated GIF or WebP.
        int frames = 1;

        [[nodiscard]] long pixels() const { return static_cast<long>(width) * height; }
    };

    // Lets another thread cut a decode short. Once requested, every load using it fails
    // until reset(). The direct decoders check between rows, libvips is told to stop.
    class Abort {

    public:
        void request();
        void reset();

        [[nodiscard]] bool requested() const { return _requested.load(std::memory_order_relaxed); }

        // For load() only.
        void arm(_VipsImage *image);
        void disarm();

    private:
        std::mutex _guard;
        std::atomic<bool> _requested = false;
        _VipsImage *_image = nullptr;
    };

    // Lowercase with the dot: what the folder lists.
    [[nodiscard]] std::span<const std::string_view> suffixes();

    // From the first bytes of the file.
    [[nodiscard]] Format sniff(std::span<const std::uint8_t> head);

    // True when the format decodes to a smaller size for a fraction of the whole file's cost,
    // so a screen sized preview is worth asking for before the full decode.
    [[nodiscard]] bool scales_cheaply(Format format);

    // True when the format is drawn from shapes, so it renders sharp at any scale.
    [[nodiscard]] bool scalable(Format format);

    // False when neither a decoder here nor libvips knows the file.
    [[nodiscard]] bool recognised(const std::filesystem::path &file);

    // Reads the header only.
    bool probe(const std::filesystem::path &file, Info *info, std::string *error = nullptr);

    // Decodes straight into an RGBA8 bitmap, oriented for display. A box smaller than the
    // image asks for the size the image would have fitted into it: formats that scale
    // cheaply deliver that size, every other one delivers the image whole and leaves the
    // shrinking to the caller. Scalable formats render to fit the box, larger or smaller.
    // Force makes every format deliver a size within the box without ever holding the image
    // whole, for images too large for that.
    enum class Fit : std::uint8_t {
        Cheap,
        Force,
    };

    bool load(const std::filesystem::path &file, int boxWidth, int boxHeight, Bitmap *out, std::string *error = nullptr, Abort *abort = nullptr, Fit fit = Fit::Cheap);

    // Part of a scalable image rendered at the scale, the part given in pixels of the image
    // at that scale.
    bool render(const std::filesystem::path &file, double scale, int x, int y, int width, int height, Bitmap *out, std::string *error = nullptr, Abort *abort = nullptr);

    struct Frame {
        Bitmap bitmap;
        // How long the frame stays up, in milliseconds.
        int delay = 0;
    };

    // Every frame of an animation, each shrunk by the same integer factor as far as it takes
    // for all of them to fit in the bytes given.
    bool load_frames(const std::filesystem::path &file, std::size_t maxBytes, std::vector<Frame> *out, std::string *error = nullptr, Abort *abort = nullptr);

    // A PNG held in memory, decoded whole.
    bool load_png_memory(std::span<const std::uint8_t> data, Bitmap *out, std::string *error = nullptr);

    // libvips starts on first use. This frees it, once nothing decodes any more.
    void shutdown();

}


#endif //TIV_IMAGE_DECODE_H
