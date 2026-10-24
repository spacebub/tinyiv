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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <print>
#include <semaphore>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vips/vips8>

#include "Corpus.h"
#include "Writers.h"
#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "image/decode/Vips.h"

namespace harness {
    namespace {
        using tiv::Decode::Format;
        using vips::VImage;
        using Writers::Bytes;
        using Writers::Rows;

        // Bumped whenever a spec changes, so an older corpus is written again.
        constexpr int GENERATOR = 2;

        constexpr std::string_view STAMP = ".stamp";

        // Many writers start threads, so a few workers keep the machine busy.
        constexpr int WORKERS = 12;
        // A huge image holds up to a few gigabytes while it is written, so one goes at a time.
        constexpr std::ptrdiff_t HEAVY_AT_ONCE = 1;
        constexpr double REPORT_EVERY = 2.0;
        constexpr double SLOW_FILE = 3.0;

        using Write = std::function<void(const std::filesystem::path &file)>;

        struct Item {
            Entry entry;
            Write write;
            bool heavy = false;
        };

        struct Dims {
            int width = 0;
            int height = 0;
        };

        // Photographs have broad light, colour casts, surfaces and fine grain. Perlin noise at
        // several scales gives the first three. Gaussian noise gives the grain. Encoders then spend
        // about what they spend on photographs. The samples are floats around 0 to 255.
        VImage photo(const int width, const int height, const int seed, const double grain = 2.5) {
            const auto perlin = [&](const int cell, const int layer) {
                return VImage::perlin(
                        width, height,
                        VImage::option()->set("cell_size", std::max(cell, 4))->set("seed", (seed * 16) + layer));
            };
            const int broad = std::max(std::max(width, height) / 3, 64);
            const VImage light = (perlin(broad, 1) * 90.0) + (perlin(broad / 3, 2) * 40.0);
            const VImage tint = perlin(broad, 3).bandjoin(perlin(broad, 4)).bandjoin(perlin(broad, 5)) * 60.0;
            const VImage cells = VImage::worley(width, height,
                                                VImage::option()->set("cell_size", 120)->set("seed", (seed * 16) + 6));
            const VImage shade = (cells * (1.0 / 60.0)).pow(0.5) * 35.0;
            const VImage detail = (perlin(23, 7) * 22.0) + (perlin(7, 8) * 9.0);
            VImage held = tint + (light + shade + detail + 100.0);

            if (grain > 0.0) {
                held = held
                       + VImage::gaussnoise(
                               width, height,
                               VImage::option()->set("sigma", grain)->set("mean", 0.0)->set("seed", (seed * 16) + 9));
            }

            return held;
        }

        VImage rgb8(const VImage &image) {
            return image.cast(VIPS_FORMAT_UCHAR);
        }

        VImage grey(const VImage &image) {
            return (image.extract_band(0) * 0.3) + (image.extract_band(1) * 0.59) + (image.extract_band(2) * 0.11);
        }

        // Clear in places, solid in others, and soft between.
        VImage alpha(const int width, const int height, const int seed) {
            return (VImage::perlin(width, height,
                                   VImage::option()
                                           ->set("cell_size", std::max(std::max(width, height) / 4, 16))
                                           ->set("seed", seed))
                    * 600.0)
                   + 128.0;
        }

        VImage deep(const VImage &image) {
            return (image * 257.0).cast(VIPS_FORMAT_USHORT);
        }

        // Gives 16 bit PQ code values. SDR white lands near 0.58 and highlights reach about 2000 nits.
        VImage pq(const VImage &image, const double top = 65535.0) {
            const VImage normal = image / 255.0;

            return (((normal * 0.55) + (normal.pow(4.0) * 0.25) + 0.05) * top).cast(VIPS_FORMAT_USHORT);
        }

        VImage hlg(const VImage &image, const double top = 65535.0) {
            return ((image / 255.0) * 0.95 * top).cast(VIPS_FORMAT_USHORT);
        }

        VImage linear(const VImage &image) {
            return ((image / 255.0).cast(VIPS_FORMAT_FLOAT).abs().pow(2.2) * 3.0).cast(VIPS_FORMAT_FLOAT);
        }

        // Packs 3-3-2 levels of red, green and blue into one index.
        VImage index332(const VImage &rgb) {
            const VImage bytes = rgb8(rgb);

            return (((bytes.extract_band(0) >> 5) << 5) | ((bytes.extract_band(1) >> 5) << 2)
                    | (bytes.extract_band(2) >> 6))
                    .cast(VIPS_FORMAT_UCHAR);
        }

        Bytes palette332() {
            Bytes held;

            for (unsigned i = 0; i < 256; ++i) {
                held.push_back(static_cast<std::uint8_t>(((i >> 5U) & 7U) * 255U / 7U));
                held.push_back(static_cast<std::uint8_t>(((i >> 2U) & 7U) * 255U / 7U));
                held.push_back(static_cast<std::uint8_t>((i & 3U) * 255U / 3U));
            }

            return held;
        }

        // How many values a sample of the depth takes.
        int levels(const int depth) {
            return static_cast<int>(1U << static_cast<unsigned>(depth));
        }

        // Evenly spread colours, for palettes of a few entries.
        Bytes palette_of(const int entries) {
            Bytes held;

            for (int i = 0; i < entries; ++i) {
                held.push_back(static_cast<std::uint8_t>((i * 255) / std::max(entries - 1, 1)));
                held.push_back(static_cast<std::uint8_t>(((i * 97) + 40) % 256));
                held.push_back(static_cast<std::uint8_t>(255 - ((i * 255) / std::max(entries - 1, 1))));
            }

            return held;
        }

        Rows rows_of(const VImage &image) {
            return [image](const int y, const int count, const std::span<std::uint8_t> out) {
                const tiv::Decode::Vips::ErrorMark mark;
                const VImage part = image.crop(0, y, image.width(), count);
                const VImage target = VImage::new_from_memory(out.data(), out.size(), image.width(), count,
                                                              image.bands(), image.format());

                if (vips_image_write(part.get_image(), target.get_image()) != 0) {
                    throw std::runtime_error(mark.since());
                }
            };
        }

        Bytes pixels(const VImage &image) {
            std::size_t size = 0;
            void *data = image.write_to_memory(&size);
            const std::span bytes(static_cast<const std::uint8_t *>(data), size);
            Bytes held(bytes.begin(), bytes.end());

            g_free(data);

            return held;
        }

        Bytes buffer(const VImage &image, const char *suffix) {
            void *data = nullptr;
            std::size_t size = 0;

            image.write_to_buffer(suffix, &data, &size);

            const std::span bytes(static_cast<const std::uint8_t *>(data), size);
            Bytes held(bytes.begin(), bytes.end());

            g_free(data);

            return held;
        }

        Bytes text(const std::string_view value) {
            return {value.begin(), value.end()};
        }

        Bytes exif_segment(const int orientation, const bool bigEndian = false) {
            Bytes held = {'E', 'x', 'i', 'f', 0, 0};

            Writers::put(held, Writers::exif(orientation, bigEndian));

            return held;
        }

        constexpr int APP1 = 0xE1;
        constexpr int APP2 = 0xE2;

        // A deterministic stream of bytes, for files that hold nothing.
        Bytes noise(const std::size_t count, std::uint32_t seed) {
            Bytes held(count);

            for (std::uint8_t &byte : held) {
                seed = (seed * 1664525U) + 1013904223U;
                byte = static_cast<std::uint8_t>(seed >> 24U);
            }

            return held;
        }

        class Plan {

        public:
            explicit Plan(const Size size) : _size(size) {}

            [[nodiscard]] bool full() const { return _size == Size::Full; }

            // Each call gives a slightly different size, so no two files share both sides.
            Dims next_dims() {
                const int k = _count++ % 16;

                return full() ? Dims{.width = 1760 + (24 * k), .height = 1240 + (16 * k)}
                              : Dims{.width = 880 + (12 * k), .height = 620 + (8 * k)};
            }

            Entry &add(const std::string_view folder, const std::string_view name, const std::string_view format,
                       const Format kind, const int width, const int height, Write write, const bool heavy = false) {
                Entry entry;

                entry.file = std::filesystem::path(folder) / name;
                entry.format = format;
                entry.kind = kind;
                entry.width = width;
                entry.height = height;
                _items.push_back({.entry = entry, .write = std::move(write), .heavy = heavy});

                return _items.back().entry;
            }

            std::vector<Item> take() { return std::move(_items); }

        private:
            Size _size;
            std::vector<Item> _items;
            int _count = 0;
        };

        void write_jpeg(const std::filesystem::path &file, const Writers::Jpeg &spec, const VImage &image) {
            Writers::save(file, Writers::jpeg(spec, rows_of(image)));
        }

        Writers::Jpeg jpeg_spec(const Dims dims) {
            return {.width = dims.width, .height = dims.height};
        }

        // The format of a file of the browse folder, by the kind the shot names.
        struct Shape {
            std::string_view format;
            Format type = Format::Jpeg;
        };

        Shape browse_shape(const std::string_view kind) {
            if (kind == "png" || kind == "rgba") {
                return {.format = "PNG", .type = Format::Png};
            }

            if (kind == "webp") {
                return {.format = "WEBP", .type = Format::WebP};
            }

            if (kind == "avif" || kind == "heic") {
                return {.format = "HEIF", .type = Format::Heif};
            }

            if (kind == "jxl") {
                return {.format = "JXL", .type = Format::Jxl};
            }

            if (kind == "tiff") {
                return {.format = "TIFF", .type = Format::Tiff};
            }

            return {.format = "JPEG", .type = Format::Jpeg};
        }

        Write browse_jpeg(const Dims dims, const int s, const std::string_view kind, const int orientation) {
            return [dims, s, kind, orientation](const std::filesystem::path &file) {
                Writers::Jpeg spec = jpeg_spec(dims);

                spec.progressive = kind == "progressive";
                spec.quality = kind == "q80" ? 80 : 90;

                if (kind == "444") {
                    spec.h = 1;
                    spec.v = 1;
                    spec.quality = 95;
                } else if (kind == "rows") {
                    spec.restart = 1;
                    spec.restartRows = true;
                } else if (kind == "interval") {
                    spec.restart = 16;
                }

                if (orientation != 1) {
                    spec.markers.emplace_back(APP1, exif_segment(orientation));
                }

                write_jpeg(file, spec, rgb8(photo(dims.width, dims.height, s)));
            };
        }

        Write browse_png(const Dims dims, const int s, const bool rgba) {
            return [dims, s, rgba](const std::filesystem::path &file) {
                const VImage colour = rgb8(photo(dims.width, dims.height, s));

                Writers::write_png(
                        file,
                        {.width = dims.width, .height = dims.height, .colour = static_cast<std::uint8_t>(rgba ? 6 : 2)},
                        rows_of(rgba ? colour.bandjoin(rgb8(alpha(dims.width, dims.height, s))) : colour));
            };
        }

        Write browse_write(const std::string_view kind, const Dims dims, const int s, const int orientation) {
            switch (browse_shape(kind).type) {
                case Format::Png:
                    return browse_png(dims, s, kind == "rgba");
                case Format::WebP:
                    return [dims, s](const std::filesystem::path &file) {
                        rgb8(photo(dims.width, dims.height, s)).write_to_file((file.string() + "[Q=82]").c_str());
                    };
                case Format::Heif:
                    return [dims, s, av1 = kind == "avif"](const std::filesystem::path &file) {
                        Writers::write_heif(file, {.width = dims.width, .height = dims.height, .av1 = av1},
                                            rows_of(rgb8(photo(dims.width, dims.height, s))));
                    };
                case Format::Jxl:
                    return [dims, s](const std::filesystem::path &file) {
                        Writers::save(file, Writers::jxl({.width = dims.width, .height = dims.height},
                                                         rows_of(rgb8(photo(dims.width, dims.height, s)))));
                    };
                case Format::Tiff:
                    return [dims, s](const std::filesystem::path &file) {
                        Writers::write_tiff(file, {.width = dims.width, .height = dims.height, .predictor = true},
                                            rows_of(rgb8(photo(dims.width, dims.height, s))));
                    };
                default:
                    return browse_jpeg(dims, s, kind, orientation);
            }
        }

        void add_browse(Plan &plan) {
            struct Shot {
                std::string_view name;
                int width;
                int height;
                // 1 to 8 for a JPEG with Exif, else the kind of file.
                std::string_view kind;
                int orientation;
            };

            constexpr std::array SHOTS = {
                    Shot{.name = "alder.jpg", .width = 4000, .height = 3000, .kind = "baseline", .orientation = 1},
                    Shot{.name = "birch.jpg", .width = 4032, .height = 3024, .kind = "progressive", .orientation = 1},
                    Shot{.name = "cedar.png", .width = 3264, .height = 2448, .kind = "png", .orientation = 1},
                    Shot{.name = "dogwood.jpg", .width = 4624, .height = 3468, .kind = "baseline", .orientation = 6},
                    Shot{.name = "elm.webp", .width = 4000, .height = 2252, .kind = "webp", .orientation = 1},
                    Shot{.name = "fir.jpg", .width = 6000, .height = 4000, .kind = "rows", .orientation = 1},
                    Shot{.name = "ginkgo.avif", .width = 3840, .height = 2160, .kind = "avif", .orientation = 1},
                    Shot{.name = "hazel.jpg", .width = 3000, .height = 4000, .kind = "444", .orientation = 1},
                    Shot{.name = "ivy.png", .width = 2880, .height = 2880, .kind = "rgba", .orientation = 1},
                    Shot{.name = "juniper.heic", .width = 4032, .height = 3024, .kind = "heic", .orientation = 1},
                    Shot{.name = "kauri.jpg", .width = 5184, .height = 3456, .kind = "baseline", .orientation = 8},
                    Shot{.name = "larch.jxl", .width = 4096, .height = 2730, .kind = "jxl", .orientation = 1},
                    Shot{.name = "maple.tif", .width = 3456, .height = 2304, .kind = "tiff", .orientation = 1},
                    Shot{.name = "nettle.jpg", .width = 3648, .height = 2736, .kind = "q80", .orientation = 1},
                    Shot{.name = "oak.webp", .width = 3000, .height = 3000, .kind = "webp", .orientation = 1},
                    Shot{.name = "pine.jpg", .width = 2448, .height = 3264, .kind = "progressive", .orientation = 1},
                    Shot{.name = "quince.jpg", .width = 5472, .height = 3648, .kind = "interval", .orientation = 1},
                    Shot{.name = "rowan.jpg", .width = 4000, .height = 6000, .kind = "baseline", .orientation = 1},
                    Shot{.name = "spruce.jpg", .width = 4256, .height = 2832, .kind = "q80", .orientation = 1},
                    Shot{.name = "tamarind.jpg", .width = 3968, .height = 2976, .kind = "rows", .orientation = 1},
            };

            int seed = 100;

            for (const Shot &shot : SHOTS) {
                const Dims dims = plan.full() ? Dims{.width = shot.width, .height = shot.height}
                                              : Dims{.width = shot.width / 4, .height = shot.height / 4};
                const bool swapped = shot.orientation >= 5;
                const Shape shape = browse_shape(shot.kind);
                Entry &entry = plan.add("browse", shot.name, shape.format, shape.type,
                                        swapped ? dims.height : dims.width, swapped ? dims.width : dims.height,
                                        browse_write(shot.kind, dims, seed++, shot.orientation));

                entry.orientation = shot.orientation;
            }
        }

        void add_jpeg_formats(Plan &plan) {
            struct Variant {
                std::string_view name;
                std::function<void(Writers::Jpeg &spec)> set;
                bool hdr = false;
            };

            const std::vector<Variant> variants = {
                    {.name = "jpeg-420.jpg", .set = [](Writers::Jpeg &) {}},
                    {.name = "jpeg-422.jpg", .set = [](Writers::Jpeg &spec) { spec.v = 1; }},
                    {
                            .name = "jpeg-444.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.h = 1;
                                        spec.v = 1;
                                    },
                    },
                    {
                            .name = "jpeg-411.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.h = 4;
                                        spec.v = 1;
                                    },
                    },
                    {
                            .name = "jpeg-440.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.h = 1;
                                        spec.v = 2;
                                    },
                    },
                    {
                            .name = "jpeg-grey.jpg",
                            .set = [](Writers::Jpeg &spec) { spec.colour = Writers::JpegColour::Grey; },
                    },
                    {
                            .name = "jpeg-cmyk.jpg",
                            .set = [](Writers::Jpeg &spec) { spec.colour = Writers::JpegColour::Cmyk; },
                    },
                    {
                            .name = "jpeg-ycck.jpg",
                            .set = [](Writers::Jpeg &spec) { spec.colour = Writers::JpegColour::Ycck; },
                    },
                    {
                            .name = "jpeg-rgb.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.colour = Writers::JpegColour::Rgb;
                                        spec.h = 1;
                                        spec.v = 1;
                                    },
                    },
                    {.name = "jpeg-progressive.jpg", .set = [](Writers::Jpeg &spec) { spec.progressive = true; }},
                    {
                            .name = "jpeg-progressive-grey.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.progressive = true;
                                        spec.colour = Writers::JpegColour::Grey;
                                    },
                    },
                    {.name = "jpeg-arithmetic.jpg", .set = [](Writers::Jpeg &spec) { spec.arithmetic = true; }},
                    {.name = "jpeg-lossless.jpg", .set = [](Writers::Jpeg &spec) { spec.lossless = true; }},
                    {.name = "jpeg-restart-interval.jpg", .set = [](Writers::Jpeg &spec) { spec.restart = 7; }},
                    {
                            .name = "jpeg-restart-rows-444.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.restart = 1;
                                        spec.restartRows = true;
                                        spec.h = 1;
                                        spec.v = 1;
                                    },
                    },
                    {
                            .name = "jpeg-restart-grey.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.restart = 2;
                                        spec.restartRows = true;
                                        spec.colour = Writers::JpegColour::Grey;
                                    },
                    },
                    {
                            .name = "jpeg-no-jfif.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        spec.jfif = false;
                                        spec.markers.emplace_back(APP1, exif_segment(1));
                                    },
                    },
                    {
                            .name = "jpeg-icc.jpg",
                            .set =
                                    [](Writers::Jpeg &spec) {
                                        VipsBlob *profile = VImage::profile_load("p3");
                                        std::size_t size = 0;
                                        const auto *data =
                                                static_cast<const std::uint8_t *>(vips_blob_get(profile, &size));
                                        Bytes body = text(std::string_view("ICC_PROFILE\0", 12));

                                        body.push_back(1);
                                        body.push_back(1);
                                        Writers::put(body, std::span(data, size));
                                        vips_area_unref(VIPS_AREA(profile));
                                        spec.markers.emplace_back(APP2, std::move(body));
                                    },
                    },
            };

            int seed = 200;

            for (const Variant &variant : variants) {
                Dims dims = plan.next_dims();
                const int s = seed++;

                // Banded decodes start at 4 megapixels.
                if (variant.name.starts_with("jpeg-restart") && plan.full()) {
                    dims = {.width = 2600, .height = 1800};
                }

                plan.add("formats", variant.name, "JPEG", Format::Jpeg, dims.width, dims.height,
                         [dims, s, set = variant.set](const std::filesystem::path &file) {
                             Writers::Jpeg spec = jpeg_spec(dims);

                             set(spec);

                             const VImage colour = rgb8(photo(dims.width, dims.height, s));
                             VImage image = colour;

                             if (spec.colour == Writers::JpegColour::Grey) {
                                 image = rgb8(grey(photo(dims.width, dims.height, s)));
                             } else if (spec.colour == Writers::JpegColour::Cmyk
                                        || spec.colour == Writers::JpegColour::Ycck) {
                                 const VImage inverse = 255 - colour;

                                 image = inverse.bandjoin((inverse.extract_band(0) / 3).cast(VIPS_FORMAT_UCHAR))
                                                 .cast(VIPS_FORMAT_UCHAR);
                             }

                             write_jpeg(file, spec, image);
                         });
            }

            // UltraHDR keeps the gain map metadata in XMP. ISO 21496-1 keeps it in a binary APP2 segment.
            for (const bool iso : {false, true}) {
                const Dims dims = plan.next_dims();
                const int s = seed++;
                Entry &entry = plan.add(
                        "formats", iso ? "jpeg-iso-gainmap.jpg" : "jpeg-ultrahdr.jpg", "JPEG", Format::Jpeg, dims.width,
                        dims.height, [dims, s, iso](const std::filesystem::path &file) {
                            const Writers::Gain gain{.alternateHeadroom = 2.5F, .max = 2.5F};
                            const Bytes primary =
                                    Writers::jpeg(jpeg_spec(dims), rows_of(rgb8(photo(dims.width, dims.height, s))));
                            const Dims small{.width = dims.width / 4, .height = dims.height / 4};
                            Writers::Jpeg mapSpec = jpeg_spec(small);

                            mapSpec.colour = Writers::JpegColour::Grey;

                            if (iso) {
                                Bytes body = text(std::string_view("urn:iso:std:iso:ts:21496:-1\0", 28));

                                Writers::put(body, Writers::gain_iso(gain));
                                mapSpec.markers.emplace_back(APP2, std::move(body));
                            } else {
                                Bytes body = text(std::string_view("http://ns.adobe.com/xap/1.0/\0", 29));

                                Writers::put(body, Writers::gain_xmp(gain));
                                mapSpec.markers.emplace_back(APP1, std::move(body));
                            }

                            const Bytes map = Writers::jpeg(
                                    mapSpec, rows_of(rgb8(grey(photo(small.width, small.height, s + 50, 0.0)))));

                            Writers::save(file, Writers::jpeg_with_gain_map(primary, map));
                        });

                entry.hdr = true;
            }
        }

        struct PngVariant {
            std::string_view name;
            std::uint8_t colour;
            int depth;
            bool interlaced = false;
            // cICP primaries and transfer, none when zero.
            int primaries = 0;
            int transfer = 0;
            bool trns = false;
            bool hdr = false;
        };

        VImage png_palette(const PngVariant &variant, const VImage &base, Writers::Png &spec) {
            VImage image;

            if (variant.depth == 8) {
                image = index332(base);
                spec.palette = palette332();
            } else {
                image = (rgb8(grey(base)) >> (8 - variant.depth)).cast(VIPS_FORMAT_UCHAR);
                spec.palette = palette_of(levels(variant.depth));
            }

            if (variant.trns) {
                for (int i = 0; i < 256; ++i) {
                    spec.transparency.push_back(static_cast<std::uint8_t>((i * 37) % 256));
                }
            }

            return image;
        }

        // Samples in the transfer and depth the variant stores.
        VImage png_transfer(const PngVariant &variant, const VImage &samples) {
            if (variant.transfer == 16) {
                return pq(samples, variant.depth == 16 ? 65535.0 : 255.0);
            }

            if (variant.transfer == 18) {
                return hlg(samples);
            }

            if (variant.transfer == 8) {
                return ((samples / 255.0).abs().pow(2.2) * 65535.0).cast(VIPS_FORMAT_USHORT);
            }

            return variant.depth == 16 ? deep(samples) : rgb8(samples);
        }

        VImage png_direct(const PngVariant &variant, const VImage &base, const Dims dims, const int s,
                          Writers::Png &spec) {
            const bool colour = variant.colour == 2 || variant.colour == 6;
            const bool withAlpha = variant.colour == 4 || variant.colour == 6;
            VImage samples = png_transfer(variant, colour ? base : grey(base));

            if (variant.depth < 8) {
                samples = (samples >> (8 - variant.depth)).cast(VIPS_FORMAT_UCHAR);
            }

            if (withAlpha) {
                const VImage clear = alpha(dims.width, dims.height, s);

                samples = samples.bandjoin(variant.depth == 16 ? deep(clear) : rgb8(clear));
            }

            if (variant.depth == 8 && samples.format() != VIPS_FORMAT_UCHAR) {
                samples = samples.cast(VIPS_FORMAT_UCHAR);
            }

            // The colour key is the value at the top left, so the image holds it.
            if (variant.trns) {
                for (const std::uint8_t value : pixels(samples.crop(0, 0, 1, 1))) {
                    Writers::put_be(spec.transparency, value, 2);
                }
            }

            return samples;
        }

        void write_png_variant(const std::filesystem::path &file, const PngVariant &variant, const Dims dims,
                               const int s) {
            Writers::Png spec{
                    .width = dims.width,
                    .height = dims.height,
                    .colour = variant.colour,
                    .depth = variant.depth,
                    .interlaced = variant.interlaced,
            };
            const VImage base = photo(dims.width, dims.height, s);
            const VImage image =
                    variant.colour == 3 ? png_palette(variant, base, spec) : png_direct(variant, base, dims, s, spec);

            if (variant.primaries != 0) {
                spec.cicp = {
                        static_cast<std::uint8_t>(variant.primaries),
                        static_cast<std::uint8_t>(variant.transfer),
                        0,
                        1,
                };
            }

            Writers::write_png(file, spec, rows_of(image));
        }

        void add_png_formats(Plan &plan) {
            constexpr std::array PNG_VARIANTS = {
                    PngVariant{.name = "png-rgb.png", .colour = 2, .depth = 8},
                    PngVariant{.name = "png-rgba.png", .colour = 6, .depth = 8},
                    PngVariant{.name = "png-grey.png", .colour = 0, .depth = 8},
                    PngVariant{.name = "png-grey-alpha.png", .colour = 4, .depth = 8},
                    PngVariant{.name = "png-grey-1bit.png", .colour = 0, .depth = 1},
                    PngVariant{.name = "png-grey-4bit.png", .colour = 0, .depth = 4},
                    PngVariant{.name = "png-grey-key.png", .colour = 0, .depth = 8, .trns = true},
                    PngVariant{.name = "png-palette.png", .colour = 3, .depth = 8},
                    PngVariant{.name = "png-palette-2bit.png", .colour = 3, .depth = 2},
                    PngVariant{.name = "png-palette-trns.png", .colour = 3, .depth = 8, .trns = true},
                    PngVariant{.name = "png-rgb-key.png", .colour = 2, .depth = 8, .trns = true},
                    PngVariant{.name = "png-grey-16.png", .colour = 0, .depth = 16},
                    PngVariant{.name = "png-rgb-16.png", .colour = 2, .depth = 16},
                    PngVariant{.name = "png-rgba-16.png", .colour = 6, .depth = 16},
                    PngVariant{.name = "png-adam7.png", .colour = 2, .depth = 8, .interlaced = true},
                    PngVariant{.name = "png-adam7-palette.png", .colour = 3, .depth = 4, .interlaced = true},
                    PngVariant{.name = "png-adam7-16.png", .colour = 6, .depth = 16, .interlaced = true},
                    PngVariant{.name = "png-cicp-p3.png", .colour = 2, .depth = 8, .primaries = 12, .transfer = 13},
                    PngVariant{
                            .name = "png-cicp-pq.png",
                            .colour = 2,
                            .depth = 16,
                            .primaries = 9,
                            .transfer = 16,
                            .hdr = true,
                    },
                    PngVariant{
                            .name = "png-cicp-pq-alpha.png",
                            .colour = 6,
                            .depth = 16,
                            .primaries = 9,
                            .transfer = 16,
                            .hdr = true,
                    },
                    PngVariant{
                            .name = "png-cicp-pq-8bit.png",
                            .colour = 2,
                            .depth = 8,
                            .primaries = 9,
                            .transfer = 16,
                            .hdr = true,
                    },
                    PngVariant{
                            .name = "png-cicp-hlg.png",
                            .colour = 2,
                            .depth = 16,
                            .primaries = 9,
                            .transfer = 18,
                            .hdr = true,
                    },
                    PngVariant{
                            .name = "png-cicp-linear.png",
                            .colour = 2,
                            .depth = 16,
                            .primaries = 1,
                            .transfer = 8,
                            .hdr = true,
                    },
                    PngVariant{
                            .name = "png-cicp-pq-adam7.png",
                            .colour = 2,
                            .depth = 16,
                            .interlaced = true,
                            .primaries = 9,
                            .transfer = 16,
                            .hdr = true,
                    },
            };

            int seed = 300;

            for (const PngVariant &variant : PNG_VARIANTS) {
                const Dims dims = plan.next_dims();
                Entry &entry = plan.add("formats", variant.name, "PNG", Format::Png, dims.width, dims.height,
                                        [dims, s = seed++, variant](const std::filesystem::path &file) {
                                            write_png_variant(file, variant, dims, s);
                                        });

                entry.hdr = variant.hdr;
            }

            // The content decides the format, whatever the name says.
            const Dims dims = plan.next_dims();

            plan.add("formats", "png-content.jpg", "PNG", Format::Png, dims.width, dims.height,
                     [dims](const std::filesystem::path &file) {
                         Writers::write_png(file, {.width = dims.width, .height = dims.height},
                                            rows_of(rgb8(photo(dims.width, dims.height, 399))));
                     });
        }

        void add_webp_formats(Plan &plan) {
            struct Variant {
                std::string_view name;
                std::string_view options;
                bool alpha;
            };

            constexpr std::array VARIANTS = {
                    Variant{.name = "webp-lossy.webp", .options = "[Q=80]", .alpha = false},
                    Variant{.name = "webp-lossless.webp", .options = "[lossless=true,effort=2]", .alpha = false},
                    Variant{.name = "webp-alpha.webp", .options = "[Q=80]", .alpha = true},
                    Variant{.name = "webp-lossless-alpha.webp", .options = "[lossless=true,effort=2]", .alpha = true},
            };

            int seed = 400;

            for (const Variant &variant : VARIANTS) {
                const Dims dims = plan.next_dims();
                const int s = seed++;

                plan.add("formats", variant.name, "WEBP", Format::WebP, dims.width, dims.height,
                         [dims, s, variant](const std::filesystem::path &file) {
                             VImage image = rgb8(photo(dims.width, dims.height, s));

                             if (variant.alpha) {
                                 image = image.bandjoin(rgb8(alpha(dims.width, dims.height, s)));
                             }

                             image.write_to_file((file.string() + std::string(variant.options)).c_str());
                         });
            }
        }

        void add_jxl_formats(Plan &plan) {
            int seed = 500;
            const auto add = [&](const std::string_view name, const Writers::Jxl &base, const bool hdr,
                                 const std::function<VImage(const VImage &)> &shape) {
                const Dims dims = plan.next_dims();
                const int s = seed++;
                const bool swapped = base.orientation >= 5;
                Entry &entry = plan.add(
                        "formats", name, "JXL", Format::Jxl, swapped ? dims.height : dims.width,
                        swapped ? dims.width : dims.height, [dims, s, base, shape](const std::filesystem::path &file) {
                            Writers::Jxl spec = base;

                            spec.width = dims.width;
                            spec.height = dims.height;

                            const int frames = std::max<int>(static_cast<int>(spec.durations.size()), 1);
                            const VImage image = shape(photo(dims.width, dims.height * frames, s));

                            Writers::save(file, Writers::jxl(spec, rows_of(image)));
                        });

                entry.hdr = hdr;
                entry.orientation = base.orientation;
            };

            add("jxl-lossy.jxl", Writers::Jxl(), false, rgb8);
            add("jxl-lossless.jxl", {.lossless = true, .effort = 2}, false, rgb8);
            add("jxl-alpha.jxl", {.channels = 4}, false, [](const VImage &image) {
                return rgb8(image).bandjoin(rgb8(alpha(image.width(), image.height(), 7)));
            });
            add("jxl-grey.jxl", {.channels = 1}, false, [](const VImage &image) { return rgb8(grey(image)); });
            add("jxl-16.jxl", {.bits = 16, .lossless = true, .effort = 2}, false, deep);
            add("jxl-pq.jxl", {.bits = 16, .colour = Writers::JxlColour::Pq}, true,
                [](const VImage &image) { return pq(image); });
            add("jxl-hlg.jxl", {.bits = 16, .colour = Writers::JxlColour::Hlg}, true,
                [](const VImage &image) { return hlg(image); });
            add("jxl-linear.jxl", {.bits = 32, .colour = Writers::JxlColour::LinearSrgb}, true, linear);
            add("jxl-orient-6.jxl", {.orientation = 6}, false, rgb8);
            add("jxl-container.jxl", {.container = true}, false, rgb8);
            // tinyiv does not play JPEG XL animations. It shows the first frame.
            add("jxl-animated.jxl", {.durations = {100, 200, 150}}, false, rgb8);

            const Dims dims = plan.next_dims();
            Entry &entry = plan.add("formats", "jxl-gainmap.jxl", "JXL", Format::Jxl, dims.width, dims.height,
                                    [dims](const std::filesystem::path &file) {
                                        const Bytes base = Writers::jxl(
                                                {
                                                        .width = dims.width,
                                                        .height = dims.height,
                                                        .bits = 16,
                                                        .colour = Writers::JxlColour::Pq,
                                                },
                                                rows_of(pq(photo(dims.width, dims.height, 590))));
                                        const Dims small{.width = dims.width / 4, .height = dims.height / 4};
                                        const Bytes map = Writers::jxl(
                                                {.width = small.width, .height = small.height, .channels = 1},
                                                rows_of(rgb8(grey(photo(small.width, small.height, 591, 0.0)))));
                                        const Writers::Gain gain{
                                                .baseHeadroom = 2.5F,
                                                .alternateHeadroom = 0.0F,
                                                .min = -2.5F,
                                                .max = 0.0F,
                                        };

                                        Writers::save(file, Writers::jxl_with_gain_map(base, gain, map));
                                    });

            entry.hdr = true;
        }

        void add_tiff_formats(Plan &plan) {
            using Writers::TiffCompression;

            struct Variant {
                std::string_view name;
                Writers::Tiff spec;
                // Turns the photo into the samples the spec asks for.
                std::string_view shape;
                bool hdr = false;
            };

            constexpr int MINISBLACK = 1;
            constexpr int PALETTE = 3;
            constexpr int SEPARATED = 5;
            constexpr int UNASSOCIATED = 2;
            constexpr int ASSOCIATED = 1;

            const std::vector<Variant> variants = {
                    {.name = "tiff-lzw.tif", .spec = {.predictor = true}, .shape = "rgb"},
                    {
                            .name = "tiff-tiles.tif",
                            .spec =
                                    {
                                            .tileWidth = 256,
                                            .tileHeight = 256,
                                            .compression = TiffCompression::Deflate,
                                            .predictor = true,
                                    },
                            .shape = "rgb",
                    },
                    {.name = "tiff-none.tif", .spec = {.compression = TiffCompression::None}, .shape = "rgb"},
                    {
                            .name = "tiff-packbits.tif",
                            .spec = {.stripRows = 1, .compression = TiffCompression::PackBits},
                            .shape = "rgb",
                    },
                    {
                            .name = "tiff-zstd.tif",
                            .spec = {.compression = TiffCompression::Zstd, .predictor = true},
                            .shape = "rgb",
                    },
                    {.name = "tiff-grey.tif", .spec = {.samples = 1, .photometric = MINISBLACK}, .shape = "grey"},
                    {
                            .name = "tiff-grey-alpha.tif",
                            .spec = {.samples = 2, .photometric = MINISBLACK, .extra = UNASSOCIATED},
                            .shape = "grey-alpha",
                    },
                    {.name = "tiff-rgba.tif", .spec = {.samples = 4, .extra = UNASSOCIATED}, .shape = "rgba"},
                    {
                            .name = "tiff-rgba-premultiplied.tif",
                            .spec = {.samples = 4, .extra = ASSOCIATED},
                            .shape = "rgba",
                    },
                    {.name = "tiff-16.tif", .spec = {.bits = 16, .predictor = true}, .shape = "rgb16"},
                    {
                            .name = "tiff-float.tif",
                            .spec = {.bits = 32, .compression = TiffCompression::Deflate, .predictor = true},
                            .shape = "float",
                            .hdr = true,
                    },
                    {.name = "tiff-planar.tif", .spec = {.planar = true}, .shape = "rgb"},
                    {.name = "tiff-cmyk.tif", .spec = {.samples = 4, .photometric = SEPARATED}, .shape = "cmyk"},
                    {.name = "tiff-palette.tif", .spec = {.samples = 1, .photometric = PALETTE}, .shape = "palette"},
                    {.name = "tiff-jpeg.tif", .spec = {.compression = TiffCompression::Jpeg}, .shape = "rgb"},
                    {.name = "tiff-big-endian.tif", .spec = {.bigEndian = true}, .shape = "rgb"},
                    {
                            .name = "tiff-tiles-edges.tif",
                            .spec = {.tileWidth = 128, .tileHeight = 64, .compression = TiffCompression::Lzw},
                            .shape = "rgb",
                    },
            };

            int seed = 600;

            for (const Variant &variant : variants) {
                const Dims dims = plan.next_dims();
                const int s = seed++;
                Entry &entry =
                        plan.add("formats", variant.name, "TIFF", Format::Tiff, dims.width, dims.height,
                                 [dims, s, variant](const std::filesystem::path &file) {
                                     Writers::Tiff spec = variant.spec;
                                     const VImage base = photo(dims.width, dims.height, s);
                                     const std::string_view shape = variant.shape;
                                     VImage image = rgb8(base);

                                     spec.width = dims.width;
                                     spec.height = dims.height;

                                     if (shape == "grey") {
                                         image = rgb8(grey(base));
                                     } else if (shape == "grey-alpha") {
                                         image = rgb8(grey(base)).bandjoin(rgb8(alpha(dims.width, dims.height, s)));
                                     } else if (shape == "rgba") {
                                         image = image.bandjoin(rgb8(alpha(dims.width, dims.height, s)));
                                     } else if (shape == "rgb16") {
                                         image = deep(base);
                                     } else if (shape == "float") {
                                         image = linear(base);
                                     } else if (shape == "cmyk") {
                                         const VImage inverse = 255 - image;

                                         image = inverse.bandjoin((inverse.extract_band(1) / 4).cast(VIPS_FORMAT_UCHAR))
                                                         .cast(VIPS_FORMAT_UCHAR);
                                     } else if (shape == "palette") {
                                         const Bytes palette = palette332();

                                         image = index332(base);

                                         for (std::size_t c = 0; c < 3; ++c) {
                                             for (std::size_t i = 0; i < 256; ++i) {
                                                 spec.colormap.push_back(
                                                         static_cast<std::uint16_t>(palette.at((i * 3) + c) * 257));
                                             }
                                         }
                                     }

                                     Writers::write_tiff(file, spec, rows_of(image));
                                 });

                entry.hdr = variant.hdr;
            }
        }

        void add_heif_formats(Plan &plan) {
            struct Variant {
                std::string_view name;
                Writers::Heif spec;
                bool hdr = false;
            };

            const std::vector<Variant> variants = {
                    {.name = "heic-plain.heic", .spec = {}},
                    {.name = "heic-alpha.heic", .spec = {.alpha = true}},
                    {
                            .name = "heic-pq.heic",
                            .spec = {.bits = 10, .primaries = 9, .transfer = 16, .matrix = 9},
                            .hdr = true,
                    },
                    {.name = "heic-grid.heic", .spec = {.columns = 3, .rows = 2}},
                    {.name = "heic-rotated.heic", .spec = {.orientation = 6}},
                    {.name = "avif-plain.avif", .spec = {.av1 = true}},
                    {.name = "avif-alpha.avif", .spec = {.av1 = true, .alpha = true}},
                    {
                            .name = "avif-pq.avif",
                            .spec = {.av1 = true, .bits = 10, .primaries = 9, .transfer = 16, .matrix = 9},
                            .hdr = true,
                    },
                    {
                            .name = "avif-hlg.avif",
                            .spec = {.av1 = true, .bits = 10, .primaries = 9, .transfer = 18, .matrix = 9},
                            .hdr = true,
                    },
                    {.name = "avif-grid.avif", .spec = {.av1 = true, .columns = 2, .rows = 2}},
            };

            int seed = 700;

            for (const Variant &variant : variants) {
                const Dims dims = plan.next_dims();
                const int s = seed++;
                // libheif applies the turn while it decodes. The image shows turned and reports no orientation.
                const bool swapped = variant.spec.orientation >= 5;
                Entry &entry = plan.add("formats", variant.name, "HEIF", Format::Heif,
                                        swapped ? dims.height : dims.width, swapped ? dims.width : dims.height,
                                        [dims, s, variant](const std::filesystem::path &file) {
                                            Writers::Heif spec = variant.spec;
                                            const VImage base = photo(dims.width, dims.height, s);
                                            VImage image;

                                            spec.width = dims.width;
                                            spec.height = dims.height;

                                            if (spec.transfer == 16) {
                                                image = pq(base, 1023.0);
                                            } else if (spec.transfer == 18) {
                                                image = hlg(base, 1023.0);
                                            } else {
                                                image = rgb8(base);
                                            }

                                            if (spec.alpha) {
                                                image = image.bandjoin(rgb8(alpha(dims.width, dims.height, s)));
                                            }

                                            Writers::write_heif(file, spec, rows_of(image));
                                        });

                entry.hdr = variant.hdr;
            }
        }

        void add_bmp_formats(Plan &plan) {
            using Writers::BmpHeader;

            constexpr std::uint32_t RLE8 = 1;
            constexpr std::uint32_t RLE4 = 2;
            constexpr std::uint32_t BITFIELDS = 3;
            constexpr std::uint32_t ALPHABITFIELDS = 6;

            struct Variant {
                std::string_view name;
                Writers::Bmp spec;
            };

            const std::vector<Variant> variants = {
                    {.name = "bmp-1bit.bmp", .spec = {.depth = 1}},
                    {.name = "bmp-2bit.bmp", .spec = {.depth = 2}},
                    {.name = "bmp-4bit.bmp", .spec = {.depth = 4}},
                    {.name = "bmp-8bit.bmp", .spec = {.depth = 8}},
                    {.name = "bmp-8bit-used.bmp", .spec = {.depth = 8, .used = 100}},
                    {
                            .name = "bmp-rle8.bmp",
                            .spec = {.depth = 8, .compression = RLE8, .deltaRow = 40, .deltaRows = 20},
                    },
                    {.name = "bmp-rle4.bmp", .spec = {.depth = 4, .compression = RLE4, .deltaRow = 10, .deltaRows = 5}},
                    {.name = "bmp-555.bmp", .spec = {.depth = 16}},
                    {
                            .name = "bmp-565.bmp",
                            .spec =
                                    {
                                            .depth = 16,
                                            .header = BmpHeader::V3,
                                            .compression = BITFIELDS,
                                            .masks = {0xF800, 0x07E0, 0x001F, 0},
                                    },
                    },
                    {
                            .name = "bmp-4444.bmp",
                            .spec =
                                    {
                                            .depth = 16,
                                            .header = BmpHeader::V3,
                                            .compression = BITFIELDS,
                                            .masks = {0x0F00, 0x00F0, 0x000F, 0xF000},
                                    },
                    },
                    {.name = "bmp-24.bmp", .spec = {}},
                    {.name = "bmp-24-top-down.bmp", .spec = {.topDown = true}},
                    {.name = "bmp-32-alpha.bmp", .spec = {.depth = 32}},
                    {.name = "bmp-32-zero-alpha.bmp", .spec = {.depth = 32}},
                    {
                            .name = "bmp-32-v5.bmp",
                            .spec =
                                    {
                                            .depth = 32,
                                            .header = BmpHeader::V5,
                                            .compression = BITFIELDS,
                                            .masks = {0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000},
                                    },
                    },
                    {
                            .name = "bmp-32-masks.bmp",
                            .spec =
                                    {
                                            .depth = 32,
                                            .header = BmpHeader::V4,
                                            .compression = BITFIELDS,
                                            .masks = {0x3FF00000, 0x000FFC00, 0x000003FF, 0},
                                    },
                    },
                    {
                            .name = "bmp-32-alphabitfields.bmp",
                            .spec =
                                    {
                                            .depth = 32,
                                            .compression = ALPHABITFIELDS,
                                            .masks = {0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000},
                                    },
                    },
                    {
                            .name = "bmp-16-bitfields-info.bmp",
                            .spec = {.depth = 16, .compression = BITFIELDS, .masks = {0x7C00, 0x03E0, 0x001F, 0}},
                    },
                    {.name = "bmp-core.bmp", .spec = {.depth = 8, .header = BmpHeader::Core}},
                    {.name = "bmp-core-24.bmp", .spec = {.header = BmpHeader::Core}},
                    {.name = "bmp-os2.bmp", .spec = {.depth = 4, .header = BmpHeader::Os2}},
                    {.name = "bmp-os2-short.bmp", .spec = {.header = BmpHeader::Os2Short}},
                    {
                            .name = "bmp-v2.bmp",
                            .spec =
                                    {
                                            .depth = 32,
                                            .header = BmpHeader::V2,
                                            .compression = BITFIELDS,
                                            .masks = {0x00FF0000, 0x0000FF00, 0x000000FF, 0},
                                    },
                    },
                    {.name = "bmp-v4-24.bmp", .spec = {.header = BmpHeader::V4}},
                    {.name = "bmp-truncated.bmp", .spec = {.truncate = 40000}},
            };

            int seed = 800;

            const auto add = [&](const std::string_view name, const Writers::Bmp &base, const Dims dims) {
                const int s = seed++;

                plan.add("formats", name, "BMP", Format::Bmp, dims.width, dims.height,
                         [dims, s, base, name](const std::filesystem::path &file) {
                             Writers::Bmp spec = base;
                             const VImage photograph = photo(dims.width, dims.height, s);
                             Bytes data;

                             spec.width = dims.width;
                             spec.height = dims.height;

                             if (spec.depth <= 8) {
                                 const auto bits = static_cast<unsigned>(spec.depth);

                                 data = pixels((rgb8(grey(photograph)) >> (8U - bits)).cast(VIPS_FORMAT_UCHAR));
                                 spec.palette = palette_of(levels(spec.depth));

                                 if (spec.used != 0) {
                                     spec.palette.resize(static_cast<std::size_t>(spec.used) * 3);
                                 }
                             } else {
                                 const VImage clear =
                                         name == "bmp-32-zero-alpha.bmp"
                                                 ? VImage::black(dims.width, dims.height).cast(VIPS_FORMAT_UCHAR)
                                                 : rgb8(alpha(dims.width, dims.height, s));

                                 data = pixels(rgb8(photograph).bandjoin(clear));
                             }

                             Writers::save(file, Writers::bmp(spec, data));
                         });
            };

            for (const Variant &variant : variants) {
                add(variant.name, variant.spec, plan.next_dims());
            }

            // Targets of 64 MB and more are written past the cache.
            add("bmp-24-large.bmp", Writers::Bmp(),
                plan.full() ? Dims{.width = 4800, .height = 3600} : plan.next_dims());
        }

        // A square icon of the photo, RGBA.
        Bytes icon_pixels(const int size, const int seed) {
            return pixels(rgb8(photo(size, size, seed)).bandjoin(rgb8(alpha(size, size, seed))));
        }

        Bytes icon_png(const int size, const int seed) {
            const Bytes rgba = icon_pixels(size, seed);

            return Writers::png({.width = size, .height = size, .colour = 6},
                                [&rgba, size](const int y, const int count, const std::span<std::uint8_t> out) {
                                    std::memcpy(out.data(), &rgba.at(static_cast<std::size_t>(y) * size * 4),
                                                static_cast<std::size_t>(count) * size * 4);
                                });
        }

        Bytes icon_dib(const int size, const int depth, const int seed) {
            Writers::Bmp spec{.width = size, .height = size, .depth = depth};

            if (depth <= 8) {
                spec.palette = palette_of(levels(depth));

                return Writers::bmp(spec,
                                    pixels((rgb8(grey(photo(size, size, seed))) >> (8U - static_cast<unsigned>(depth)))
                                                   .cast(VIPS_FORMAT_UCHAR)),
                                    true);
            }

            return Writers::bmp(spec, icon_pixels(size, seed), true);
        }

        void add_icon_formats(Plan &plan) {
            plan.add("formats", "ico-png.ico", "ICO", Format::Ico, 256, 256, [](const std::filesystem::path &file) {
                Writers::save(file, Writers::ico({
                                            {.width = 16, .height = 16, .depth = 4, .data = icon_dib(16, 4, 901)},
                                            {.width = 48, .height = 48, .depth = 32, .data = icon_dib(48, 32, 902)},
                                            {.width = 256, .height = 256, .depth = 32, .data = icon_png(256, 903)},
                                    }));
            });
            plan.add("formats", "ico-dib.ico", "ICO", Format::Ico, 64, 64, [](const std::filesystem::path &file) {
                Writers::save(file, Writers::ico({
                                            {.width = 16, .height = 16, .depth = 1, .data = icon_dib(16, 1, 904)},
                                            {.width = 32, .height = 32, .depth = 8, .data = icon_dib(32, 8, 905)},
                                            {.width = 64, .height = 64, .depth = 32, .data = icon_dib(64, 32, 906)},
                                    }));
            });
            plan.add("formats", "ico-dib-24-mask.ico", "ICO", Format::Ico, 48, 48,
                     [](const std::filesystem::path &file) {
                         Writers::save(
                                 file,
                                 Writers::ico(
                                         {{.width = 48, .height = 48, .depth = 24, .data = icon_dib(48, 24, 907)}}));
                     });
            plan.add("formats", "icns-png.icns", "ICNS", Format::Icns, 256, 256, [](const std::filesystem::path &file) {
                const Bytes small = icon_pixels(16, 908);
                Bytes mask;

                for (std::size_t i = 3; i < small.size(); i += 4) {
                    mask.push_back(small.at(i));
                }

                Writers::save(file, Writers::icns({
                                            {"is32", Writers::icns_packed(small)},
                                            {"s8mk", mask},
                                            {"ic08", icon_png(256, 909)},
                                    }));
            });
            plan.add("formats", "icns-packed.icns", "ICNS", Format::Icns, 128, 128,
                     [](const std::filesystem::path &file) {
                         std::vector<std::pair<std::string, Bytes>> chunks;

                         for (const auto &[tag, maskTag, size] :
                              {std::tuple{"il32", "l8mk", 32}, std::tuple{"it32", "t8mk", 128}}) {
                             const Bytes rgba = icon_pixels(size, 910 + size);
                             Bytes planes = std::string_view(tag) == "it32" ? Bytes{0, 0, 0, 0} : Bytes{};
                             Bytes mask;

                             Writers::put(planes, Writers::icns_packed(rgba));

                             for (std::size_t i = 3; i < rgba.size(); i += 4) {
                                 mask.push_back(rgba.at(i));
                             }

                             chunks.emplace_back(tag, std::move(planes));
                             chunks.emplace_back(maskTag, std::move(mask));
                         }

                         Writers::save(file, Writers::icns(chunks));
                     });
            plan.add("formats", "icns-jp2.icns", "ICNS", Format::Icns, 512, 512, [](const std::filesystem::path &file) {
                const Bytes jp2 = buffer(rgb8(photo(512, 512, 912)), ".jp2");

                Writers::save(file, Writers::icns({{"ic07", icon_png(128, 913)}, {"ic09", jp2}}));
            });
        }

        Bytes gif_indices(const VImage &rgb) {
            const VImage bytes = rgb8(rgb);

            return pixels((((bytes.extract_band(0) >> 6) << 4) | ((bytes.extract_band(1) >> 6) << 2)
                           | (bytes.extract_band(2) >> 6))
                                  .cast(VIPS_FORMAT_UCHAR));
        }

        Bytes palette64() {
            Bytes held;

            for (unsigned i = 0; i < 64; ++i) {
                held.push_back(static_cast<std::uint8_t>(((i >> 4U) & 3U) * 85U));
                held.push_back(static_cast<std::uint8_t>(((i >> 2U) & 3U) * 85U));
                held.push_back(static_cast<std::uint8_t>((i & 3U) * 85U));
            }

            return held;
        }

        void add_other_formats(Plan &plan) {
            const Dims a = plan.next_dims();

            plan.add("formats", "gif-static.gif", "GIF", Format::Gif, a.width, a.height,
                     [a](const std::filesystem::path &file) {
                         Writers::save(
                                 file,
                                 Writers::gif({
                                         .width = a.width,
                                         .height = a.height,
                                         .palette = palette64(),
                                         .loop = -1,
                                         .frames =
                                                 {
                                                         {
                                                                 .width = a.width,
                                                                 .height = a.height,
                                                                 .indices = gif_indices(photo(a.width, a.height, 950)),
                                                                 .delay = 0,
                                                         },
                                                 },
                                 }));
                     });

            const Dims b = plan.next_dims();

            plan.add("formats", "gif-interlaced.gif", "GIF", Format::Gif, b.width, b.height,
                     [b](const std::filesystem::path &file) {
                         Writers::save(
                                 file,
                                 Writers::gif({
                                         .width = b.width,
                                         .height = b.height,
                                         .palette = palette64(),
                                         .loop = -1,
                                         .frames =
                                                 {
                                                         {
                                                                 .width = b.width,
                                                                 .height = b.height,
                                                                 .indices = gif_indices(photo(b.width, b.height, 951)),
                                                                 .delay = 0,
                                                                 .interlaced = true,
                                                         },
                                                 },
                                 }));
                     });

            struct Saved {
                std::string_view name;
                std::string_view format;
                std::string_view shape;
                bool hdr;
            };

            constexpr std::array SAVED = {
                    Saved{.name = "radiance.hdr", .format = "HDR", .shape = "float", .hdr = true},
                    Saved{.name = "portable.ppm", .format = "PPM", .shape = "rgb", .hdr = false},
                    Saved{.name = "portable-grey.pgm", .format = "PPM", .shape = "grey", .hdr = false},
                    Saved{.name = "portable-float.pfm", .format = "PPM", .shape = "float", .hdr = true},
                    Saved{.name = "flexible.fits", .format = "FITS", .shape = "grey", .hdr = false},
                    Saved{.name = "native.v", .format = "VIPS", .shape = "rgb", .hdr = false},
                    Saved{.name = "jpeg2000.jp2", .format = "JP2", .shape = "rgb", .hdr = false},
            };

            int seed = 960;

            for (const Saved &saved : SAVED) {
                const Dims dims = plan.next_dims();
                const int s = seed++;
                Entry &entry = plan.add("formats", saved.name, saved.format, Format::Other, dims.width, dims.height,
                                        [dims, s, saved](const std::filesystem::path &file) {
                                            const VImage base = photo(dims.width, dims.height, s);
                                            VImage image = rgb8(base);

                                            if (saved.shape == "grey") {
                                                image = rgb8(grey(base));
                                            } else if (saved.shape == "float") {
                                                image = linear(base);
                                            }

                                            image.copy_memory().write_to_file(file.string().c_str());
                                        });

                entry.hdr = saved.hdr;
            }
        }

        // Covers each disposal method, a patch with transparency, a local palette and an interlaced frame.
        void write_disposal_gif(const std::filesystem::path &file, const int w, const int h) {
            Writers::Gif spec{.width = w, .height = h, .palette = palette64(), .loop = 0};
            const int pw = w / 2;
            const int ph = h / 2;

            spec.frames.push_back({.width = w, .height = h, .indices = gif_indices(photo(w, h, 970)), .delay = 5});

            for (int k = 1; k <= 3; ++k) {
                Bytes patch = gif_indices(photo(pw, ph, 970 + k));

                for (std::size_t i = 0; i < patch.size(); i += 7) {
                    patch.at(i) = 63;
                }

                spec.frames.push_back({
                        .x = k * w / 8,
                        .y = k * h / 8,
                        .width = pw,
                        .height = ph,
                        .indices = std::move(patch),
                        .delay = 10 * k,
                        .disposal = k,
                        .transparent = 63,
                });
            }

            Bytes local = palette64();

            std::ranges::reverse(local);
            spec.frames.push_back({
                    .width = w,
                    .height = h,
                    .indices = gif_indices(photo(w, h, 975)),
                    .delay = 80,
                    .palette = local,
            });
            spec.frames.push_back({
                    .width = w,
                    .height = h,
                    .indices = gif_indices(photo(w, h, 976)),
                    .delay = 7,
                    .interlaced = true,
            });
            Writers::save(file, Writers::gif(spec));
        }

        // Zero delays test how the player treats them. Without a loop extension the animation plays once.
        void write_timed_gif(const std::filesystem::path &file, const int w, const int h, const bool looped) {
            Writers::Gif spec{.width = w, .height = h, .palette = palette64(), .loop = looped ? 3 : -1};

            for (int k = 0; k < (looped ? 3 : 4); ++k) {
                spec.frames.push_back({
                        .width = w,
                        .height = h,
                        .indices = gif_indices(photo(w, h, 980 + k)),
                        .delay = looped ? 0 : 15 + (10 * k),
                });
            }

            Writers::save(file, Writers::gif(spec));
        }

        void write_animated_webp(const std::filesystem::path &file, const int w, const int h, const bool lossless,
                                 const int frames) {
            VImage strip = rgb8(photo(w, h * frames, lossless ? 990 : 991));

            if (lossless) {
                strip = strip.bandjoin(rgb8(alpha(w, h * frames, 992)));
            }

            strip = strip.copy_memory();
            strip.set("page-height", h);
            strip.set("delay", lossless ? std::vector<int>{120, 60, 240} : std::vector<int>{40, 80, 120, 160, 200});
            strip.set("loop", 0);
            strip.write_to_file((file.string() + (lossless ? "[lossless=true,effort=2]" : "[Q=75]")).c_str());
        }

        void add_animated(Plan &plan) {
            const Dims dims = plan.full() ? Dims{.width = 800, .height = 600} : Dims{.width = 320, .height = 240};
            const int w = dims.width;
            const int h = dims.height;

            plan.add("animated", "anim-disposal.gif", "GIF", Format::Gif, w, h,
                     [w, h](const std::filesystem::path &file) { write_disposal_gif(file, w, h); })
                    .frames = 6;

            for (const bool looped : {true, false}) {
                plan.add("animated", looped ? "anim-zero-delay.gif" : "anim-once.gif", "GIF", Format::Gif, w, h,
                         [w, h, looped](const std::filesystem::path &file) { write_timed_gif(file, w, h, looped); })
                        .frames = looped ? 3 : 4;
            }

            for (const bool lossless : {false, true}) {
                const int frames = lossless ? 3 : 5;

                plan.add("animated", lossless ? "anim-lossless-alpha.webp" : "anim-lossy.webp", "WEBP", Format::WebP, w,
                         h,
                         [w, h, lossless, frames](const std::filesystem::path &file) {
                             write_animated_webp(file, w, h, lossless, frames);
                         })
                        .frames = frames;
            }
        }

        std::string svg_body(const int seed) {
            std::string held;

            for (int i = 0; i < 40; ++i) {
                const int x = ((i * 137) + (seed * 31)) % 760;
                const int y = ((i * 89) + (seed * 17)) % 560;
                const int r = 10 + ((i * 53) % 70);

                held += std::format(R"svg(<circle cx="{}" cy="{}" r="{}" fill="url(#g{})" opacity="0.8"/>)svg", x, y, r,
                                    i % 2);
                held += '\n';
            }

            held += R"(<path d="M 20 500 C 200 300, 400 700, 780 420" stroke="#203040" stroke-width="9" fill="none"/>)";
            held += R"(<text x="40" y="80" font-family="sans-serif" font-size="48" fill="#102030">tinyiv</text>)";

            return held;
        }

        std::string_view svg_defs() {
            return R"(<defs><linearGradient id="g0" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e05a20"/>)"
                   R"(<stop offset="1" stop-color="#2050c0"/></linearGradient><radialGradient id="g1">)"
                   R"(<stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#30a050"/></radialGradient>)"
                   R"(<filter id="soft"><feGaussianBlur stdDeviation="12"/></filter></defs>)";
        }

        struct Drawing {
            std::string_view name;
            std::string head;
            int width;
            int height;
            bool blur = false;
        };

        void write_drawing(const std::filesystem::path &file, const Drawing &drawing, const int s) {
            const bool prefixed = drawing.head.contains("<svg:svg");
            const std::string body = std::string(svg_defs()) + (drawing.blur ? R"svg(<g filter="url(#soft)">)svg" : "")
                                     + svg_body(s) + (drawing.blur ? "</g>" : "");

            Writers::save(file, text(drawing.head + "\n" + body + (prefixed ? "</svg:svg>\n" : "</svg>\n")));
        }

        void write_photo_pdf(const std::filesystem::path &file, const int pw, const int ph) {
            const int iw = pw / 2;
            const int ih = ph / 3;
            const Bytes jpeg = Writers::jpeg({.width = iw, .height = ih}, rows_of(rgb8(photo(iw, ih, 1001))));
            std::string content = std::format("q {} 0 0 {} {} {} cm /Photo Do Q\n", iw, ih, pw / 4, ph / 2);

            content += "BT /F1 36 Tf 40 60 Td (tinyiv harness page) Tj ET\n";

            for (int i = 0; i < 30; ++i) {
                content += std::format("{:.2f} {:.2f} {:.2f} rg {} {} {} {} re f\n", (i % 5) / 5.0, (i % 3) / 3.0,
                                       (i % 7) / 7.0, (i * 37) % pw, (i * 53) % (ph / 2), 20 + ((i % 9) * 6),
                                       15 + ((i % 4) * 9));
            }

            Writers::save(file, Writers::pdf({
                                        {
                                                .width = static_cast<double>(pw),
                                                .height = static_cast<double>(ph),
                                                .content = content,
                                                .photo = jpeg,
                                                .photoWidth = iw,
                                                .photoHeight = ih,
                                        },
                                }));
        }

        void write_pages_pdf(const std::filesystem::path &file, const int pw, const int ph) {
            std::vector<Writers::PdfPage> pages;

            for (int p = 0; p < 3; ++p) {
                std::string content = std::format("BT /F1 48 Tf 60 100 Td (page {}) Tj ET\n", p + 1);

                for (int i = 0; i < 200; ++i) {
                    content += std::format("{} {} m {} {} l {} w S\n", (i * 7) % ph, (i * 13) % pw, (i * 29) % ph,
                                           (i * 41) % pw, 1 + (i % 4));
                }

                pages.push_back({
                        .width = static_cast<double>(p == 1 ? pw : ph),
                        .height = static_cast<double>(p == 1 ? ph : pw),
                        .content = content,
                        .deflate = true,
                });
            }

            Writers::save(file, Writers::pdf(pages));
        }

        void add_vector(Plan &plan) {
            const int w = plan.full() ? 1600 : 800;
            const int h = plan.full() ? 1200 : 600;

            const std::vector<Drawing> drawings = {
                    {
                            .name = "drawing.svg",
                            .head = std::format(
                                    R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 800 600">)",
                                    w, h),
                            .width = w,
                            .height = h,
                    },
                    {
                            .name = "view-box-only.svg",
                            .head = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 600">)",
                            .width = 800,
                            .height = 600,
                    },
                    {
                            .name = "no-view-box.svg",
                            .head = R"(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600">)",
                            .width = 800,
                            .height = 600,
                    },
                    {
                            .name = "slice.svg",
                            .head = std::format(
                                    R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="100 50 600 600" )"
                                    R"(preserveAspectRatio="xMaxYMin slice">)",
                                    w, h),
                            .width = w,
                            .height = h,
                    },
                    {
                            .name = "stretched.svg",
                            .head = std::format(
                                    R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 800 600" )"
                                    R"(preserveAspectRatio="none">)",
                                    w, h / 2),
                            .width = w,
                            .height = h / 2,
                    },
                    {
                            .name = "prolog.svg",
                            .head = std::format(
                                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- drawn for tinyiv -->\n"
                                    "<!DOCTYPE svg:svg [ <!ENTITY name \"tinyiv\"> ]>\n"
                                    R"(<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns="http://www.w3.org/2000/svg" )"
                                    R"(width='{}' height='{}' viewBox='0,0,800,600'>)",
                                    h, w),
                            .width = h,
                            .height = w,
                    },
                    {
                            .name = "blur.svg",
                            .head = std::format(
                                    R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 800 600">)",
                                    w, h),
                            .width = w,
                            .height = h,
                            .blur = true,
                    },
            };

            int seed = 1;

            for (const Drawing &drawing : drawings) {
                const int s = seed++;

                plan.add("vector", drawing.name, "SVG", Format::Svg, drawing.width, drawing.height,
                         [drawing, s](const std::filesystem::path &file) { write_drawing(file, drawing, s); });
            }

            const int pw = plan.full() ? 1200 : 600;
            const int ph = plan.full() ? 1600 : 800;

            plan.add("vector", "page.pdf", "PDF", Format::Pdf, pw, ph,
                     [pw, ph](const std::filesystem::path &file) { write_photo_pdf(file, pw, ph); });

            plan.add("vector", "pages.pdf", "PDF", Format::Pdf, ph, pw,
                     [pw, ph](const std::filesystem::path &file) { write_pages_pdf(file, pw, ph); });
        }

        struct OrientVariant {
            std::string name;
            std::string_view format;
            Format kind;
            // 0 means no metadata at all, and -1 means metadata without the tag.
            int tag;
            bool bigEndian = false;
            std::string_view flavour{};
        };

        std::vector<OrientVariant> oriented_variants() {
            std::vector<OrientVariant> variants;

            for (int o = 1; o <= 8; ++o) {
                variants.push_back({
                        .name = std::format("orient-jpeg-{}.jpg", o),
                        .format = "JPEG",
                        .kind = Format::Jpeg,
                        .tag = o,
                });
                variants.push_back(
                        {.name = std::format("orient-png-{}.png", o), .format = "PNG", .kind = Format::Png, .tag = o});
                variants.push_back({
                        .name = std::format("orient-webp-{}.webp", o),
                        .format = "WEBP",
                        .kind = Format::WebP,
                        .tag = o,
                });
                variants.push_back({
                        .name = std::format("orient-tiff-{}.tif", o),
                        .format = "TIFF",
                        .kind = Format::Tiff,
                        .tag = o,
                });
            }

            variants.push_back({.name = "orient-jpeg-bare.jpg", .format = "JPEG", .kind = Format::Jpeg, .tag = 0});
            variants.push_back({.name = "orient-jpeg-untagged.jpg", .format = "JPEG", .kind = Format::Jpeg, .tag = -1});
            variants.push_back({
                    .name = "orient-jpeg-5-mm.jpg",
                    .format = "JPEG",
                    .kind = Format::Jpeg,
                    .tag = 5,
                    .bigEndian = true,
            });
            variants.push_back({.name = "orient-png-bare.png", .format = "PNG", .kind = Format::Png, .tag = 0});
            variants.push_back({.name = "orient-png-untagged.png", .format = "PNG", .kind = Format::Png, .tag = -1});
            variants.push_back({.name = "orient-webp-bare.webp", .format = "WEBP", .kind = Format::WebP, .tag = 0});
            variants.push_back({
                    .name = "orient-webp-lossless-bare.webp",
                    .format = "WEBP",
                    .kind = Format::WebP,
                    .tag = 0,
                    .flavour = "lossless",
            });
            variants.push_back({
                    .name = "orient-webp-alpha-bare.webp",
                    .format = "WEBP",
                    .kind = Format::WebP,
                    .tag = 0,
                    .flavour = "alpha",
            });
            variants.push_back(
                    {.name = "orient-webp-untagged.webp", .format = "WEBP", .kind = Format::WebP, .tag = -1});
            variants.push_back({.name = "orient-tiff-bare.tif", .format = "TIFF", .kind = Format::Tiff, .tag = 0});
            variants.push_back({
                    .name = "orient-tiff-7-mm.tif",
                    .format = "TIFF",
                    .kind = Format::Tiff,
                    .tag = 7,
                    .bigEndian = true,
            });

            return variants;
        }

        Bytes oriented_webp(const OrientVariant &variant, const VImage &image, const int w, const int h, const int s) {
            if (variant.flavour == "lossless") {
                return buffer(image, ".webp[lossless=true,effort=2,keep=none]");
            }

            if (variant.flavour == "alpha") {
                return buffer(image.bandjoin(rgb8(alpha(w, h, s))), ".webp[Q=80,keep=none]");
            }

            return buffer(image, ".webp[Q=80,keep=none]");
        }

        void write_oriented(const std::filesystem::path &file, const OrientVariant &variant, const int w, const int h,
                            const int s) {
            const VImage image = rgb8(photo(w, h, s));
            const int exifTag = variant.tag < 0 ? 0 : variant.tag;
            const bool hasExif = variant.tag != 0;

            switch (variant.kind) {
                case Format::Jpeg: {
                    Writers::Jpeg spec = jpeg_spec({.width = w, .height = h});

                    if (hasExif) {
                        spec.markers.emplace_back(APP1, exif_segment(exifTag, variant.bigEndian));
                    }

                    write_jpeg(file, spec, image);
                    break;
                }
                case Format::Png: {
                    Writers::Png spec{.width = w, .height = h};

                    if (hasExif) {
                        spec.exif = Writers::exif(exifTag);
                    }

                    Writers::write_png(file, spec, rows_of(image));
                    break;
                }
                case Format::WebP: {
                    const Bytes simple = oriented_webp(variant, image, w, h, s);

                    Writers::save(file, hasExif ? Writers::webp_with_exif(simple, Writers::exif(exifTag)) : simple);
                    break;
                }
                default:
                    Writers::write_tiff(file,
                                        {
                                                .width = w,
                                                .height = h,
                                                .orientation = variant.tag,
                                                .bigEndian = variant.bigEndian,
                                        },
                                        rows_of(image));
                    break;
            }
        }

        void add_oriented(Plan &plan) {
            const Dims dims = plan.full() ? Dims{.width = 1200, .height = 800} : Dims{.width = 480, .height = 320};
            const int w = dims.width;
            const int h = dims.height;
            int seed = 1100;

            for (const OrientVariant &variant : oriented_variants()) {
                const int orientation = std::max(variant.tag, 1);
                const bool swapped = orientation >= 5;

                plan.add("oriented", variant.name, variant.format, variant.kind, swapped ? h : w, swapped ? w : h,
                         [w, h, s = seed++, variant](const std::filesystem::path &file) {
                             write_oriented(file, variant, w, h, s);
                         })
                        .orientation = orientation;
            }
        }

        void add_huge(Plan &plan) {
            using Writers::TiffCompression;

            const Dims dims = plan.full() ? Dims{.width = 13000, .height = 10000} : Dims{.width = 6000, .height = 4500};
            const int w = dims.width;
            const int h = dims.height;
            // Lossless files of photographs this large would fill the disk, so their grain is fine.
            constexpr double LOSSLESS_GRAIN = 0.8;

            plan.add(
                    "huge", "huge-rgb.png", "PNG", Format::Png, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_png(file, {.width = w, .height = h, .level = 4, .idat = 1U << 18U},
                                           rows_of(rgb8(photo(w, h, 1201, LOSSLESS_GRAIN))));
                    },
                    true);
            plan.add(
                    "huge", "huge-16.png", "PNG", Format::Png, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_png(file, {.width = w, .height = h, .depth = 16, .level = 4, .idat = 1U << 20U},
                                           rows_of(deep(photo(w, h, 1202, LOSSLESS_GRAIN))));
                    },
                    true);
            plan.add(
                    "huge", "huge-palette.png", "PNG", Format::Png, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_png(file,
                                           {.width = w, .height = h, .colour = 3, .palette = palette332(), .level = 4},
                                           rows_of(index332(photo(w, h, 1203, LOSSLESS_GRAIN))));
                    },
                    true);

            for (const bool restart : {true, false}) {
                plan.add(
                        "huge", restart ? "huge-restart.jpg" : "huge-plain.jpg", "JPEG", Format::Jpeg, w, h,
                        [w, h, restart](const std::filesystem::path &file) {
                            Writers::Jpeg spec = jpeg_spec({.width = w, .height = h});

                            spec.quality = 85;
                            spec.restart = restart ? 1 : 0;
                            spec.restartRows = restart;
                            write_jpeg(file, spec, rgb8(photo(w, h, restart ? 1204 : 1205)));
                        },
                        true);
            }

            plan.add(
                    "huge", "huge-tiled.tif", "TIFF", Format::Tiff, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_tiff(file,
                                            {
                                                    .width = w,
                                                    .height = h,
                                                    .tileWidth = 512,
                                                    .tileHeight = 512,
                                                    .compression = TiffCompression::Deflate,
                                                    .predictor = true,
                                            },
                                            rows_of(rgb8(photo(w, h, 1206, LOSSLESS_GRAIN))));
                    },
                    true);
            plan.add(
                    "huge", "huge-strips.tif", "TIFF", Format::Tiff, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_tiff(file, {.width = w, .height = h, .stripRows = 32, .predictor = true},
                                            rows_of(rgb8(photo(w, h, 1207, LOSSLESS_GRAIN))));
                    },
                    true);
            plan.add(
                    "huge", "huge-grid.heic", "HEIF", Format::Heif, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::write_heif(
                                file,
                                {.width = w, .height = h, .columns = (w + 1023) / 1024, .rows = (h + 1023) / 1024},
                                rows_of(rgb8(photo(w, h, 1208))));
                    },
                    true);
            plan.add(
                    "huge", "huge.webp", "WEBP", Format::WebP, w, h,
                    [w, h](const std::filesystem::path &file) {
                        rgb8(photo(w, h, 1209)).write_to_file((file.string() + "[Q=80,effort=2]").c_str());
                    },
                    true);
            plan.add(
                    "huge", "huge.jxl", "JXL", Format::Jxl, w, h,
                    [w, h](const std::filesystem::path &file) {
                        Writers::save(file, Writers::jxl({.width = w, .height = h, .distance = 1.5F, .effort = 1},
                                                         rows_of(rgb8(photo(w, h, 1210)))));
                    },
                    true);
        }

        void add_broken(Plan &plan) {
            struct Broken {
                std::string_view name;
                std::function<Bytes()> make;
            };

            const auto valid_jpeg = [] {
                return Writers::jpeg({.width = 640, .height = 480}, rows_of(rgb8(photo(640, 480, 1300))));
            };
            const auto valid_png = [] {
                return Writers::png({.width = 640, .height = 480, .idat = 8192}, rows_of(rgb8(photo(640, 480, 1301))));
            };
            const auto magic = [](const std::string_view head, const std::uint32_t seed) {
                Bytes held = text(head);

                Writers::put(held, noise(4096, seed));

                return held;
            };

            const std::vector<Broken> files = {
                    {
                            .name = "broken-truncated-header.jpg",
                            .make =
                                    [valid_jpeg] {
                                        const Bytes whole = valid_jpeg();

                                        return Bytes(whole.begin(), whole.begin() + 120);
                                    },
                    },
                    {
                            .name = "broken-truncated.png",
                            .make =
                                    [valid_png] {
                                        const Bytes whole = valid_png();

                                        return Bytes(whole.begin(),
                                                     whole.begin() + static_cast<std::ptrdiff_t>(whole.size() / 2));
                                    },
                    },
                    {
                            .name = "broken-crc.png",
                            .make =
                                    [valid_png] {
                                        Bytes whole = valid_png();

                                        std::uint8_t &flipped = whole.at(whole.size() / 2);

                                        flipped = static_cast<std::uint8_t>(flipped ^ 0x5AU);

                                        return whole;
                                    },
                    },
                    {.name = "broken-empty.png", .make = [] { return Bytes{}; }},
                    {.name = "broken-empty.jpg", .make = [] { return Bytes{}; }},
                    {.name = "broken-random.jpg", .make = [] { return noise(4096, 1); }},
                    {.name = "broken-garbage.jpg", .make = [magic] { return magic("\xFF\xD8\xFF\xE0", 2); }},
                    {.name = "broken-garbage.png", .make = [magic] { return magic("\x89PNG\r\n\x1A\n", 3); }},
                    {.name = "broken-garbage.gif", .make = [magic] { return magic("GIF89a", 4); }},
                    {
                            .name = "broken-garbage.webp",
                            .make = [magic] { return magic(std::string_view("RIFF\x08\x10\0\0WEBPVP8 ", 16), 5); },
                    },
                    {.name = "broken-garbage.bmp", .make = [magic] { return magic("BM", 6); }},
                    {
                            .name = "broken-garbage.tif",
                            .make = [] { return text(std::string_view("II*\0\xFF\xFF\xFF\x0F", 8)); },
                    },
                    {
                            .name = "broken-garbage.heic",
                            .make =
                                    [magic] {
                                        return magic(std::string_view("\0\0\0\x18"
                                                                      "ftypheic\0\0\0\0mif1heic",
                                                                      24),
                                                     7);
                                    },
                    },
                    {.name = "broken-garbage.jxl", .make = [magic] { return magic("\xFF\x0A", 8); }},
                    {
                            .name = "broken-garbage.svg",
                            .make = [] { return text(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" <<<)"); },
                    },
                    {.name = "broken-garbage.pdf", .make = [magic] { return magic("%PDF-1.4\n", 9); }},
                    {
                            .name = "broken-garbage.ico",
                            .make =
                                    [] {
                                        return Bytes{
                                                0, 0,  1, 0,    1,    0, 32, 32,   0, 0, 1,
                                                0, 32, 0, 0xFF, 0xFF, 0, 0,  0x16, 0, 0, 0,
                                        };
                                    },
                    },
                    {
                            .name = "broken-garbage.icns",
                            .make =
                                    [magic] {
                                        return magic(std::string_view("icns\0\0\0\x10ic08\0\0\x7F\xFF", 16), 10);
                                    },
                    },
                    {
                            .name = "broken-bmp-depth.bmp",
                            .make =
                                    [] {
                                        Bytes whole = Writers::bmp({.width = 64, .height = 64},
                                                                   pixels(rgb8(photo(64, 64, 1302)).bandjoin(255)));

                                        whole.at(28) = 7;

                                        return whole;
                                    },
                    },
            };

            // libjpeg shows the part of a cut scan that arrived and warns, so the file opens.
            plan.add("formats", "jpeg-truncated-scan.jpg", "JPEG", Format::Jpeg, 640, 480,
                     [valid_jpeg](const std::filesystem::path &file) {
                         const Bytes whole = valid_jpeg();

                         Writers::save(file, std::span(whole).first(whole.size() / 2));
                     });

            for (const Broken &broken : files) {
                Entry &entry = plan.add(
                        "broken", broken.name, "", Format::Other, 0, 0,
                        [make = broken.make](const std::filesystem::path &file) { Writers::save(file, make()); });

                entry.broken = true;
            }
        }

        std::vector<Item> plan_for(const Size size) {
            Plan plan(size);

            add_browse(plan);
            add_jpeg_formats(plan);
            add_png_formats(plan);
            add_webp_formats(plan);
            add_jxl_formats(plan);
            add_tiff_formats(plan);
            add_heif_formats(plan);
            add_bmp_formats(plan);
            add_icon_formats(plan);
            add_other_formats(plan);
            add_animated(plan);
            add_vector(plan);
            add_oriented(plan);
            add_huge(plan);
            add_broken(plan);

            return plan.take();
        }

        std::string stamp_text(const Size size) {
            return std::format("tiv_harness corpus {} {}\n", GENERATOR, size == Size::Full ? "full" : "small");
        }

        std::string read_text(const std::filesystem::path &file) {
            std::ifstream in(file, std::ios::binary);

            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        // Fixed times an hour apart keep the order by time the same on every run. Browse files get
        // times in a different order than their names.
        std::filesystem::file_time_type time_for(const std::size_t index, const bool browse) {
            using namespace std::chrono;

            constexpr std::size_t SHUFFLE = 7;
            const std::size_t slot = browse ? (index * SHUFFLE) % 20 : index;
            const sys_seconds base = sys_days{year{2025} / March / 1};

            return clock_cast<file_clock>(base + hours(static_cast<long>(slot)));
        }

        double seconds_since(const std::chrono::steady_clock::time_point start) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        std::string joined(const std::set<std::string> &names) {
            std::string held;

            for (const std::string &name : names) {
                held += held.empty() ? name : ", " + name;
            }

            return held;
        }

        // Sets the fixed modification times and returns the bytes written.
        std::uintmax_t settle_times(const std::filesystem::path &root, const std::vector<Item> &items) {
            std::uintmax_t bytes = 0;
            std::error_code missing;
            std::size_t browsed = 0;

            for (std::size_t i = 0; i < items.size(); ++i) {
                const std::filesystem::path file = root / items.at(i).entry.file;
                const bool browse = items.at(i).entry.file.parent_path() == "browse";

                std::filesystem::last_write_time(file, time_for(browse ? browsed++ : i, browse), missing);
                bytes += std::filesystem::file_size(file, missing);
            }

            return bytes;
        }

        // Writes every item on a few threads. Heavy items go first, one at a time.
        void write_all(const std::filesystem::path &root, std::vector<Item> &items, std::vector<std::string> *failed) {
            using Clock = std::chrono::steady_clock;

            std::vector<std::size_t> order(items.size());

            for (std::size_t i = 0; i < order.size(); ++i) {
                order.at(i) = i;
            }

            std::ranges::stable_partition(order, [&items](const std::size_t i) { return items.at(i).heavy; });

            std::atomic<std::size_t> next = 0;
            std::atomic<std::size_t> done = 0;
            std::counting_semaphore<HEAVY_AT_ONCE> heavy(HEAVY_AT_ONCE);
            std::mutex guard;
            std::set<std::string> writing;
            std::atomic<bool> finished = false;
            const Clock::time_point start = Clock::now();

            std::println("writing {} files under {}", items.size(), root.string());

            const auto work = [&] {
                for (std::size_t at = next++; at < order.size(); at = next++) {
                    const Item &item = items.at(order.at(at));
                    const std::string name = item.entry.file.generic_string();
                    const Clock::time_point began = Clock::now();

                    if (item.heavy) {
                        heavy.acquire();
                    }

                    {
                        const std::scoped_lock hold(guard);

                        writing.insert(name);
                    }

                    try {
                        item.write(root / item.entry.file);
                    } catch (const std::exception &trouble) {
                        const std::scoped_lock hold(guard);

                        failed->push_back(std::format("{} was not written: {}", name, trouble.what()));
                    }

                    if (item.heavy) {
                        heavy.release();
                    }

                    const double took = seconds_since(began);
                    const std::scoped_lock hold(guard);

                    writing.erase(name);
                    ++done;

                    if (took >= SLOW_FILE) {
                        std::println("  {} took {:.1f} s", name, took);
                    }
                }
            };

            std::jthread reporter([&] {
                double reported = 0.0;

                while (!finished) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    if (const double now = seconds_since(start); now - reported >= REPORT_EVERY && !finished) {
                        reported = now;

                        const std::scoped_lock hold(guard);

                        std::println("  {}/{} written after {:.0f} s, writing {}", done.load(), items.size(), now,
                                     joined(writing));
                    }
                }
            });

            {
                std::vector<std::jthread> workers;

                workers.reserve(WORKERS);
                for (int i = 0; i < WORKERS; ++i) {
                    workers.emplace_back(work);
                }
            }

            finished = true;
            reporter.join();

            const std::uintmax_t bytes = settle_times(root, items);

            std::println("wrote {} files, {:.0f} MB, in {:.1f} s", items.size(), static_cast<double>(bytes) / 1e6,
                         seconds_since(start));
        }

        // Expected values come from the specs, so a decoder that reads a file wrongly shows up here.
        void check(const std::filesystem::path &root, const Entry &entry, std::vector<std::string> *failed) {
            const std::filesystem::path file = root / entry.file;
            const std::string name = entry.file.generic_string();
            tiv::Decode::Info info;
            std::string error;

            if (entry.broken) {
                tiv::Bitmap bitmap;

                if (tiv::Decode::probe(file, &info, &error) && tiv::Decode::load(file, 256, 256, &bitmap, &error)) {
                    failed->push_back(name + " opens but should not");
                }

                return;
            }

            if (!tiv::Decode::probe(file, &info, &error)) {
                failed->push_back(std::format("{} does not probe: {}", name, error));

                return;
            }

            std::vector<std::string> wrong;

            if (info.format != entry.format) {
                wrong.push_back(std::format("format {} not {}", info.format, entry.format));
            }

            if (info.kind != entry.kind) {
                wrong.push_back(
                        std::format("kind {} not {}", static_cast<int>(info.kind), static_cast<int>(entry.kind)));
            }

            if (info.width != entry.width || info.height != entry.height) {
                wrong.push_back(std::format("{}x{} not {}x{}", info.width, info.height, entry.width, entry.height));
            }

            if (info.orientation != entry.orientation) {
                wrong.push_back(std::format("orientation {} not {}", info.orientation, entry.orientation));
            }

            if (info.hdr != entry.hdr) {
                wrong.emplace_back(entry.hdr ? "not HDR" : "HDR");
            }

            if (info.frames != entry.frames) {
                wrong.push_back(std::format("{} frames not {}", info.frames, entry.frames));
            }

            if (entry.frames > 1) {
                constexpr std::size_t FRAME_BYTES = std::size_t{256} << 20U;
                std::vector<tiv::Decode::Frame> frames;

                if (!tiv::Decode::load_frames(file, FRAME_BYTES, &frames, &error)) {
                    wrong.push_back("frames do not load: " + error);
                } else if (std::cmp_not_equal(frames.size(), entry.frames)) {
                    wrong.push_back(std::format("{} frames load, not {}", frames.size(), entry.frames));
                }
            }

            for (const std::string &what : wrong) {
                failed->push_back(std::format("{}: {}", name, what));
            }
        }

        // A file the corpus does not list would show up in the sittings as a stranger.
        void check_strangers(const std::filesystem::path &root, const std::vector<Item> &items,
                             std::vector<std::string> *failed) {
            std::set<std::filesystem::path> known;

            for (const Item &item : items) {
                known.insert(item.entry.file);
            }

            for (const std::string_view folder :
                 {"browse", "formats", "oriented", "animated", "vector", "huge", "broken"}) {
                std::error_code failure;

                for (const auto &found : std::filesystem::directory_iterator(root / folder, failure)) {
                    const std::filesystem::path relative = std::filesystem::path(folder) / found.path().filename();

                    if (found.is_regular_file() && !known.contains(relative)) {
                        failed->push_back(relative.generic_string() + " is not in the corpus, remove it by hand");
                    }
                }
            }
        }
    }

    std::vector<Entry> prepare_corpus(const std::filesystem::path &root, const Size size,
                                      std::vector<std::string> *failed) {
        tiv::Decode::Vips::ensure();

        std::vector<Item> items = plan_for(size);
        std::set<std::filesystem::path> names;

        for (const Item &item : items) {
            if (!names.insert(item.entry.file.filename()).second) {
                failed->push_back(item.entry.file.filename().string() + " is in the corpus twice");
            }
        }

        if (read_text(root / STAMP) != stamp_text(size)) {
            for (const std::string_view folder :
                 {"browse", "formats", "oriented", "animated", "vector", "huge", "broken"}) {
                std::filesystem::create_directories(root / folder);
            }

            const std::size_t before = failed->size();

            write_all(root, items, failed);

            if (failed->size() == before) {
                std::ofstream(root / STAMP, std::ios::trunc) << stamp_text(size);
            }
        } else {
            std::println("corpus already written for this size");
        }

        const std::chrono::steady_clock::time_point checking = std::chrono::steady_clock::now();
        std::mutex guard;
        std::atomic<std::size_t> next = 0;

        {
            std::vector<std::jthread> workers;

            workers.reserve(WORKERS);
            for (int i = 0; i < WORKERS; ++i) {
                workers.emplace_back([&] {
                    for (std::size_t at = next++; at < items.size(); at = next++) {
                        std::vector<std::string> found;

                        check(root, items.at(at).entry, &found);

                        const std::scoped_lock hold(guard);

                        failed->insert(failed->end(), found.begin(), found.end());
                    }
                });
            }
        }

        check_strangers(root, items, failed);
        std::println("checked {} files in {:.1f} s", items.size(), seconds_since(checking));

        std::vector<Entry> entries;

        entries.reserve(items.size());

        for (Item &item : items) {
            entries.push_back(std::move(item.entry));
        }

        return entries;
    }
}
