// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_HARNESS_WRITERS_H
#define TIV_HARNESS_WRITERS_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace harness::Writers {
    using Bytes = std::vector<std::uint8_t>;

    // Writes count rows starting at y into out. Samples are interleaved, and 16 bit and float
    // samples use native byte order. Several threads call it at once.
    using Rows = std::function<void(int y, int count, std::span<std::uint8_t> out)>;

    void save(const std::filesystem::path &file, std::span<const std::uint8_t> bytes);

    void put(Bytes &out, std::string_view text);
    void put(Bytes &out, std::span<const std::uint8_t> bytes);
    void put_le(Bytes &out, std::uint32_t value, int bytes);
    void put_be(Bytes &out, std::uint32_t value, int bytes);

    // Builds a TIFF structured Exif block without the "Exif" prefix that JPEG adds. It holds a few
    // tags besides the orientation, some with values out of line. Orientation 0 leaves the tag out.
    Bytes exif(int orientation, bool bigEndian = false);

    struct Png {
        int width = 0;
        int height = 0;
        // The colour type and bit depth of IHDR.
        std::uint8_t colour = 2;
        int depth = 8;
        bool interlaced = false;
        // Chunk bodies, each left out when empty.
        Bytes palette{};
        Bytes transparency{};
        Bytes cicp{};
        Bytes exif{};
        int level = 6;
        // The most compressed bytes an IDAT holds.
        std::size_t idat = 65536;
    };

    // Below 8 bits the rows give one byte a sample, and the writer packs them. Rows of an image
    // that is not interlaced are deflated in parallel.
    Bytes png(const Png &spec, const Rows &rows);
    void write_png(const std::filesystem::path &file, const Png &spec, const Rows &rows);

    enum class JpegColour : std::uint8_t {
        Grey,
        YCbCr,
        // RGB stored as is, flagged by an Adobe marker.
        Rgb,
        Cmyk,
        Ycck,
    };

    struct Jpeg {
        int width = 0;
        int height = 0;
        JpegColour colour = JpegColour::YCbCr;
        // Sampling of the first component. The others take one.
        int h = 2;
        int v = 2;
        int quality = 90;
        bool progressive = false;
        bool arithmetic = false;
        bool lossless = false;
        // MCUs between restart markers, or MCU rows when restartRows is set. Zero writes no markers.
        int restart = 0;
        bool restartRows = false;
        bool jfif = true;
        // Segments written after the JFIF segment, each as a marker and a body.
        std::vector<std::pair<int, Bytes>> markers{};
    };

    // Rows give grey, RGB or CMYK as the colour stores it. YCbCr takes RGB rows and YCCK takes CMYK rows.
    Bytes jpeg(const Jpeg &spec, const Rows &rows);

    // How a gain map turns the base rendition into the alternate one, in stops. ISO 21496-1.
    struct Gain {
        float baseHeadroom = 0.0F;
        float alternateHeadroom = 2.0F;
        float min = 0.0F;
        float max = 2.0F;
        float gamma = 1.0F;
        float baseOffset = 1.0F / 64.0F;
        float alternateOffset = 1.0F / 64.0F;
        bool baseColours = true;
    };

    // The binary form, as libavif writes it. A leading version byte when versioned.
    Bytes gain_iso(const Gain &gain, bool versioned = false);
    // The hdrgm XMP packet of an UltraHDR gain map image.
    std::string gain_xmp(const Gain &gain);

    // The primary JPEG with an MPF index pointing at the gain map JPEG, which follows it.
    Bytes jpeg_with_gain_map(std::span<const std::uint8_t> primary, std::span<const std::uint8_t> map);

    enum class TiffCompression : std::uint8_t {
        None,
        Lzw,
        Deflate,
        PackBits,
        Zstd,
        // YCbCr subsampled and JPEG compressed, from RGB rows.
        Jpeg,
    };

    struct Tiff {
        int width = 0;
        int height = 0;
        int samples = 3;
        // 8, 16, or 32 for float.
        int bits = 8;
        // PHOTOMETRIC_ of tiff.h.
        int photometric = 2;
        // EXTRASAMPLE_ of tiff.h for the last sample, -1 for none.
        int extra = -1;
        bool planar = false;
        int tileWidth = 0;
        int tileHeight = 0;
        int stripRows = 64;
        TiffCompression compression = TiffCompression::Lzw;
        bool predictor = false;
        // Zero leaves the tag out.
        int orientation = 0;
        bool bigEndian = false;
        // Red, then green, then blue, for a palette.
        std::vector<std::uint16_t> colormap{};
    };

    void write_tiff(const std::filesystem::path &file, const Tiff &spec, const Rows &rows);

    struct Heif {
        int width = 0;
        int height = 0;
        bool av1 = false;
        bool alpha = false;
        // 8, or 10 with samples in 16 bit rows.
        int bits = 8;
        int quality = 60;
        // An nclx box with these code points of ITU-T H.273, when primaries is not zero.
        int primaries = 0;
        int transfer = 0;
        int matrix = 0;
        // As EXIF numbers it, written as irot and imir.
        int orientation = 1;
        // Above one each, the image is a grid of tiles.
        int columns = 1;
        int rows = 1;
    };

    void write_heif(const std::filesystem::path &file, const Heif &spec, const Rows &rows);

    enum class JxlColour : std::uint8_t {
        Srgb,
        LinearSrgb,
        Pq,
        Hlg,
    };

    struct Jxl {
        int width = 0;
        int height = 0;
        // Grey, grey and alpha, RGB, RGBA.
        int channels = 3;
        // 8, 16, or 32 for float.
        int bits = 8;
        bool lossless = false;
        float distance = 1.0F;
        int effort = 3;
        JxlColour colour = JxlColour::Srgb;
        int orientation = 1;
        // Milliseconds of each frame, an animation when more than one. Frame f takes rows from f * height.
        std::vector<int> durations{};
        bool container = false;
    };

    // A bare codestream unless the container is asked for.
    Bytes jxl(const Jxl &spec, const Rows &rows);
    // A container around the codestream with a jhgm box holding the gain map codestream.
    Bytes jxl_with_gain_map(std::span<const std::uint8_t> codestream, const Gain &gain,
                            std::span<const std::uint8_t> map);

    struct GifFrame {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        // One palette index a pixel.
        Bytes indices{};
        // Hundredths of a second.
        int delay = 10;
        int disposal = 0;
        int transparent = -1;
        // RGB triples, the global palette when empty.
        Bytes palette{};
        bool interlaced = false;
    };

    struct Gif {
        int width = 0;
        int height = 0;
        Bytes palette{};
        // NETSCAPE loop count, -1 for no loop extension.
        int loop = 0;
        std::vector<GifFrame> frames{};
    };

    Bytes gif(const Gif &spec);

    enum class BmpHeader : std::uint32_t {
        Core = 12,
        Os2Short = 16,
        Info = 40,
        V2 = 52,
        V3 = 56,
        Os2 = 64,
        V4 = 108,
        V5 = 124,
    };

    struct Bmp {
        int width = 0;
        int height = 0;
        int depth = 24;
        BmpHeader header = BmpHeader::Info;
        // A BI_ value of wingdi.h, such as 0 RGB, 1 RLE8, 2 RLE4, 3 BITFIELDS or 6 ALPHABITFIELDS.
        std::uint32_t compression = 0;
        // Red, green, blue and alpha, for bit fields and for 16 bit.
        std::array<std::uint32_t, 4> masks{};
        bool topDown = false;
        // RGB triples.
        Bytes palette{};
        // biClrUsed, zero for the whole table.
        std::uint32_t used = 0;
        // A delta escape skips deltaRows run length coded rows, starting at deltaRow.
        int deltaRow = 0;
        int deltaRows = 0;
        // Cut the file this many bytes short.
        std::size_t truncate = 0;
    };

    // Pixels are one index a byte up to 8 bits and RGBA above, top row first. An icon entry has no
    // file header. A transparency mask follows the pixels. It comes from the alpha, or below 16 bits
    // from where the index is 0.
    Bytes bmp(const Bmp &spec, std::span<const std::uint8_t> pixels, bool icon = false);

    struct IcoEntry {
        int width = 0;
        int height = 0;
        int depth = 32;
        Bytes data{};
    };

    Bytes ico(const std::vector<IcoEntry> &entries);

    // The three colour planes of RGBA pixels in the run length scheme of ICNS.
    Bytes icns_packed(std::span<const std::uint8_t> rgba);
    Bytes icns(const std::vector<std::pair<std::string, Bytes>> &chunks);

    struct PdfPage {
        double width = 0.0;
        double height = 0.0;
        // Drawing operators.
        std::string content{};
        // A JPEG drawn as /Photo, when not empty.
        Bytes photo{};
        int photoWidth = 0;
        int photoHeight = 0;
        bool deflate = false;
    };

    Bytes pdf(const std::vector<PdfPage> &pages);

    // Turns a simple WebP into an extended one, with the EXIF chunk after the image.
    Bytes webp_with_exif(std::span<const std::uint8_t> simple, std::span<const std::uint8_t> exif);
}


#endif //TIV_HARNESS_WRITERS_H
