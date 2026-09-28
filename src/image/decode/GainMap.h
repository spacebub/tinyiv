// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_DECODE_GAIN_MAP_H
#define TIV_DECODE_GAIN_MAP_H


#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "image/Bitmap.h"

namespace tiv::GainMap {

    // How a gain map turns the base rendition into the alternate one, as ISO 21496-1 puts it.
    // Gains and headrooms are in stops, log2 of the ratio to SDR white.
    struct Metadata {
        int channels = 1;
        std::array<float, 3> min{};
        std::array<float, 3> max{};
        std::array<float, 3> gamma{1.0F, 1.0F, 1.0F};
        std::array<float, 3> baseOffset{1.0F / 64.0F, 1.0F / 64.0F, 1.0F / 64.0F};
        std::array<float, 3> alternateOffset{1.0F / 64.0F, 1.0F / 64.0F, 1.0F / 64.0F};
        float baseHeadroom = 0.0F;
        float alternateHeadroom = 0.0F;
        // Otherwise in the alternate rendition's.
        bool inBaseColours = true;

        // How much of the gain a display with this headroom takes, negative where the
        // alternate rendition is the dimmer one.
        [[nodiscard]] float weight(float headroom) const;
    };

    // The binary form, with or without the version byte an AVIF tmap item leads with.
    bool parse_iso(std::span<const std::uint8_t> data, Metadata *out);

    // The hdrgm namespace of Adobe's gain map XMP: https://helpx.adobe.com/camera-raw/using/gain-map.html
    // False for a base rendition that is HDR, whose gains that form stores inverted.
    bool parse_xmp(std::string_view xmp, Metadata *out);

    struct Jpeg {
        // The gain map, a JPEG of its own after the primary image.
        std::span<const std::uint8_t> image;
        Metadata metadata;
    };

    // Through the MPF index of an UltraHDR or ISO 21496-1 JPEG: CIPA DC-007.
    bool find_jpeg(std::span<const std::uint8_t> data, Jpeg *out);

    struct Jxl {
        // A JPEG XL codestream of its own.
        std::span<const std::uint8_t> image;
        Metadata metadata;
        // The alternate rendition's primaries, where the gains apply unless in the base's.
        // Only the default encoding, sRGB, is read, and gains in any other are refused.
        bool alternateSrgb = true;
    };

    // The body of a jhgm box, laid out as libjxl writes it:
    // https://github.com/libjxl/libjxl/blob/main/lib/extras/gain_map.cc
    bool read_jxl(std::span<const std::uint8_t> box, Jxl *out);

    // Samples the map across an image of the given size, as far as the weight takes it.
    class Applier {

    public:
        // The map is borrowed and must outlive the applier.
        Applier(const Metadata &metadata, const Bitmap *map, int width, int height, float weight);

        // Linear RGBA of row y from x on, in the colour space the gains apply in.
        void apply(int x, int y, std::span<float> rgba) const;

    private:
        Metadata _metadata;
        const Bitmap *_map;
        float _scaleX;
        float _scaleY;
        // 2 to the gain, by the map's value in fine steps, for each channel.
        std::array<std::vector<float>, 3> _factors;
    };

}


#endif //TIV_DECODE_GAIN_MAP_H
