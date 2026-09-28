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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <vips/vips8>

#include "image/Bitmap.h"
#include "image/Mapped.h"
#include "image/Tone.h"
#include "image/decode/Heif.h"
#include "image/decode/Jxl.h"
#include "image/decode/Png.h"
#include "image/decode/Support.h"
#include "image/decode/Vips.h"

namespace tiv::Decode {
    namespace {
        using vips::VImage;

        struct Name {
            std::string_view loader;
            std::string_view format;
        };

        constexpr std::array NAMES = {
                Name{"jpeg", "JPEG"},
                Name{"png", "PNG"},
                Name{"webp", "WEBP"},
                Name{"tiff", "TIFF"},
                Name{"heif", "HEIF"},
                Name{"jxl", "JXL"},
                Name{"gif", "GIF"},
                Name{"svg", "SVG"},
                Name{"pdf", "PDF"},
                Name{"jp2k", "JP2"},
                Name{"dcraw", "RAW"},
                Name{"openexr", "EXR"},
                Name{"rad", "HDR"},
                Name{"ppm", "PPM"},
                Name{"vips", "VIPS"},
        };

        void drop_log(const gchar * /*domain*/, GLogLevelFlags /*level*/, const gchar * /*message*/, gpointer /*data*/) {
        }

        struct VipsState {
            std::once_flag once;
            bool started = false;
        };

        VipsState &vips_state() {
            static VipsState held;

            return held;
        }

        // Pages of these formats are frames, where in a TIFF or a PDF they are separate images.
        bool animates(const Decode::Format kind) {
            return kind == Decode::Format::Gif || kind == Decode::Format::WebP;
        }

        std::string vips_format_name(const VImage &image) {
            if (image.get_typeof(VIPS_META_LOADER) == 0) {
                return {};
            }

            std::string_view loader = image.get_string(VIPS_META_LOADER);

            if (const std::size_t cut = loader.find("load"); cut != std::string_view::npos) {
                loader = loader.substr(0, cut);
            }

            for (const Name &name : NAMES) {
                if (name.loader == loader) {
                    return std::string(name.format);
                }
            }

            std::string held(loader);

            for (char &c : held) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }

            return held;
        }

        VImage to_rgba(VImage image) {
            if (image.interpretation() != VIPS_INTERPRETATION_sRGB) {
                image = image.colourspace(VIPS_INTERPRETATION_sRGB);
            }

            if (image.format() != VIPS_FORMAT_UCHAR) {
                image = image.cast(VIPS_FORMAT_UCHAR);
            }

            if (!image.has_alpha()) {
                image = image.addalpha();
            }

            if (image.bands() > Bitmap::CHANNELS) {
                image = image.extract_band(0, VImage::option()->set("n", Bitmap::CHANNELS));
            }

            return image;
        }

        bool floating(const VImage &image) {
            return image.coding() == VIPS_CODING_RAD || image.format() == VIPS_FORMAT_FLOAT || image.format() == VIPS_FORMAT_DOUBLE;
        }

        // Where libvips hands over the rows of an HDR image, in order, for the tone mapper.
        struct MappedSink {
            const Tone::Mapper *mapper = nullptr;
            std::uint8_t *target = nullptr;
            int bands = 0;
        };

        // A region this large maps on several threads, since libvips hands regions over one at a time.
        constexpr std::size_t PARALLEL_PIXELS = std::size_t{1} << 20;

        template <typename Sample>
        void map_rows(VipsRegion *region, const VipsRect *area, const MappedSink *sink, const int from, const int to) {
            const auto samples = static_cast<std::size_t>(area->width) * static_cast<std::size_t>(sink->bands);
            const std::size_t pitch = static_cast<std::size_t>(area->width) * Bitmap::CHANNELS;

            for (int y = from; y < to; ++y) {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast): libvips hands the region over as raw rows.
                const std::span in(reinterpret_cast<const Sample *>(VIPS_REGION_ADDR(region, area->left, y)), samples);
                const std::span out(sink->target + (pitch * static_cast<std::size_t>(y)), pitch);
                // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)

                sink->mapper->map(in, sink->bands, out);
            }
        }

        template <typename Sample>
        int map_region(VipsRegion *region, VipsRect *area, void *opaque) {
            const auto *sink = static_cast<const MappedSink *>(opaque);
            const std::size_t pixels = static_cast<std::size_t>(area->width) * static_cast<std::size_t>(area->height);
            const int wanted = std::min(MAX_THREADS, static_cast<int>(std::thread::hardware_concurrency()));
            const int threads = pixels >= PARALLEL_PIXELS ? std::clamp(wanted, 1, area->height) : 1;
            const int slice = (area->height + threads - 1) / threads;
            const int bottom = area->top + area->height;

            {
                std::vector<std::jthread> workers;

                for (int from = area->top + slice; from < bottom; from += slice) {
                    workers.emplace_back(map_rows<Sample>, region, area, sink, from, std::min(from + slice, bottom));
                }

                map_rows<Sample>(region, area, sink, area->top, std::min(area->top + slice, bottom));
            }

            return 0;
        }
    }

    void Vips::ensure() {
        VipsState &state = vips_state();

        std::call_once(state.once, [&state] {
            // libvips warns about every optional module missing from the system, on every launch.
            g_log_set_handler("VIPS", G_LOG_LEVEL_WARNING, drop_log, nullptr);

            if (VIPS_INIT("tinyiv") != 0) {
                return;
            }

            // The operation cache would keep decoded images alive after the viewer has moved on.
            vips_cache_set_max(0);

            // Decoders are single threaded, so a pool the size of the machine mostly waits.
            vips_concurrency_set(std::min(vips_concurrency_get(), MAX_THREADS));

            state.started = true;
        });
    }

    std::string Vips::last_error() {
        std::string held = vips_error_buffer();

        vips_error_clear();

        while (!held.empty() && held.back() == '\n') {
            held.pop_back();
        }

        return held;
    }

    void Vips::shutdown() {
        if (vips_state().started) {
            vips_shutdown();
        }
    }

    Vips::Prepared Vips::prepare(VImage image, Tone::Source source, const Tone::Display &display) {
        if (image.coding() == VIPS_CODING_RAD) {
            image = image.rad2float();
        }

        if (floating(image)) {
            source = {Tone::Transfer::Linear, Tone::Primaries::Bt709};
        }

        if (!source.hdr()) {
            return {to_rgba(image), std::nullopt, Bitmap::Encoding::Srgb};
        }

        if (image.bands() > Bitmap::CHANNELS) {
            image = image.extract_band(0, VImage::option()->set("n", Bitmap::CHANNELS));
        }

        if (source.transfer == Tone::Transfer::Linear) {
            image = image.cast(VIPS_FORMAT_FLOAT);
        } else if (image.format() == VIPS_FORMAT_UCHAR) {
            image = image.linear(257.0, 0.0).cast(VIPS_FORMAT_USHORT);
        } else if (image.format() != VIPS_FORMAT_USHORT) {
            image = image.cast(VIPS_FORMAT_USHORT);
        }

        return {image, Tone::Mapper(source, display), display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb};
    }

    bool Vips::write_rows(const Prepared &prepared, const int y, const int count, std::uint8_t *target, Decode::Abort *abort) {
        const VImage &image = prepared.image;
        const VImage part = y == 0 && count == image.height() ? image : image.crop(0, y, image.width(), count);

        if (abort != nullptr) {
            abort->arm(part.get_image());
        }

        bool written = false;

        if (prepared.mapper) {
            // One pass, which sequential loaders need. The wide samples never take more than the regions in flight.
            MappedSink sink{&*prepared.mapper, target, image.bands()};

            written = vips_sink_disc(part.get_image(), image.format() == VIPS_FORMAT_FLOAT ? map_region<float> : map_region<std::uint16_t>, &sink) == 0;
        } else {
            const std::size_t bytes = static_cast<std::size_t>(image.width()) * Bitmap::CHANNELS * static_cast<std::size_t>(count);
            const VImage packed = VImage::new_from_memory(target, bytes, image.width(), count, Bitmap::CHANNELS, VIPS_FORMAT_UCHAR);

            written = vips_image_write(part.get_image(), packed.get_image()) == 0;
        }

        if (abort != nullptr) {
            abort->disarm();
        }

        return written;
    }

    bool Vips::write_rgba(const Prepared &prepared, Bitmap *out, Decode::Abort *abort) {
        Bitmap held = Bitmap::allocate(prepared.image.width(), prepared.image.height(), prepared.encoding);

        if (!write_rows(prepared, 0, held.height(), held.data(), abort)) {
            return false;
        }

        *out = std::move(held);

        return true;
    }

    bool Vips::probe(const std::filesystem::path &file, Decode::Info *info, std::string *error) {
        ensure();

        try {
            const VImage image = VImage::new_from_file(file.string().c_str());
            const bool swapped = vips_image_get_orientation_swap(image.get_image()) != 0;

            info->width = swapped ? image.height() : image.width();
            info->height = swapped ? image.width() : image.height();
            info->orientation = vips_image_get_orientation(image.get_image());
            info->frames = animates(info->kind) ? std::max(vips_image_get_n_pages(image.get_image()), 1) : 1;
            info->hdr = info->hdr || floating(image);

            if (info->format.empty()) {
                info->format = vips_format_name(image);
            }

            return true;
        } catch (const vips::VError &) {
            fail(error, file, last_error());

            return false;
        }
    }

    bool Vips::load(const std::filesystem::path &file, const int boxWidth, const int boxHeight, Bitmap *out, std::string *error, Decode::Abort *abort, const Via via, const Tone::Source source,
                    const Tone::Display &display) {
        ensure();

        try {
            VImage image;

            if (via == Via::Thumbnail || via == Via::Scaled) {
                const VipsSize size = via == Via::Scaled ? VIPS_SIZE_BOTH : VIPS_SIZE_DOWN;
                const bool swapped = vips_image_get_orientation_swap(VImage::new_from_file(file.string().c_str()).get_image()) != 0;

                image = VImage::thumbnail(file.string().c_str(), swapped ? boxHeight : boxWidth,
                                          VImage::option()->set("height", swapped ? boxWidth : boxHeight)->set("size", size)->set("no_rotate", true));
            } else {
                image = VImage::new_from_file(file.string().c_str(), VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL));

                if (via == Via::Shrink) {
                    const bool swapped = vips_image_get_orientation_swap(image.get_image()) != 0;
                    const int factor = shrink_factor(image.width(), image.height(), swapped ? boxHeight : boxWidth, swapped ? boxWidth : boxHeight);

                    if (factor > 1) {
                        image = image.shrink(factor, factor);
                    }
                }
            }

            if (!write_rgba(prepare(image, source, display), out, abort)) {
                fail(error, file, last_error());

                return false;
            }

            return true;
        } catch (const vips::VError &) {
            if (abort != nullptr) {
                abort->disarm();
            }

            fail(error, file, last_error());

            return false;
        }
    }

    bool Vips::load_buffer(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, Decode::Abort *abort) {
        ensure();

        try {
            if (!write_rgba(prepare(VImage::new_from_buffer(data.data(), data.size(), ""), {}), out, abort)) {
                fail(error, file, last_error());

                return false;
            }

            return true;
        } catch (const vips::VError &) {
            if (abort != nullptr) {
                abort->disarm();
            }

            fail(error, file, last_error());

            return false;
        }
    }

    Tone::Source Vips::container_tone(const Decode::Format kind, const std::span<const std::uint8_t> data) {
        switch (kind) {
            case Decode::Format::Png:
                return Png::tone(data);
            case Decode::Format::Jxl:
                return Jxl::tone(data);
            case Decode::Format::Heif:
                return Heif::colour(data).tone;
            default:
                break;
        }

        return {};
    }

    Tone::Source Vips::container_tone(const std::filesystem::path &file) {
        Mapped mapped;

        return Mapped::open(file, &mapped) ? container_tone(Decode::sniff(mapped.data()), mapped.data()) : Tone::Source{};
    }
}
