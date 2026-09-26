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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vips/vips8>

#include "support/Corpus.h"

namespace bench {
    namespace {
        using vips::VImage;

        constexpr std::size_t MIB = std::size_t{1024} * 1024;

        // Lossy encoders are so efficient that the weight would need absurd dimensions.
        constexpr long MAX_PIXELS = 64L * 1000 * 1000;
        constexpr int MAX_SIDE = 16383;

        constexpr double LOSSLESS_NOISE = 18.0;
        constexpr double LOSSY_NOISE = 45.0;

        constexpr std::array<Corpus::Spec, 10> SPECS = {{
                {"noise.png", "[compression=6]", 45 * MIB, 0, 0, LOSSLESS_NOISE},
                {"noise.jpg", "[Q=95]", 45 * MIB, 0, 0, LOSSY_NOISE},
                {"noise.webp", "[Q=90]", 45 * MIB, 0, 0, LOSSY_NOISE},
                {"noise-lossless.webp", "[lossless=true,effort=2]", 45 * MIB, 0, 0, LOSSLESS_NOISE},
                {"noise.jxl", "[distance=1,effort=5]", 45 * MIB, 0, 0, LOSSY_NOISE},
                {"noise.avif", "[Q=60,effort=1]", 45 * MIB, 0, 0, LOSSY_NOISE},
                {"noise.tif", "[compression=lzw]", 45 * MIB, 0, 0, LOSSLESS_NOISE},
                {"small.jpg", "[Q=90]", 0, 1600, 1200, LOSSY_NOISE},
                {"small.png", "[compression=6]", 0, 1600, 1200, LOSSLESS_NOISE},
                {"huge-lossless.webp", "[lossless=true,effort=0]", 0, 11547, 8660, LOSSLESS_NOISE},
        }};

        std::filesystem::path corpusDir;

        std::once_flag vipsOnce;

        void ensure_vips() {
            std::call_once(vipsOnce, [] {
                g_log_set_handler("VIPS", G_LOG_LEVEL_WARNING, [](const gchar *, GLogLevelFlags, const gchar *, gpointer) {}, nullptr);
                VIPS_INIT("tiv_bench");
            });
        }

        VImage texture(const int width, const int height, const double sigma) {
            const VImage r = VImage::perlin(width, height, VImage::option()->set("cell_size", 300)->set("seed", 1)->set("uchar", true));
            const VImage g = VImage::perlin(width, height, VImage::option()->set("cell_size", 180)->set("seed", 2)->set("uchar", true));
            const VImage b = VImage::perlin(width, height, VImage::option()->set("cell_size", 90)->set("seed", 3)->set("uchar", true));
            const VImage noise = VImage::gaussnoise(width, height, VImage::option()->set("sigma", sigma)->set("mean", 0.0)->set("seed", 4));

            return (r.bandjoin(g).bandjoin(b) + noise).cast(VIPS_FORMAT_UCHAR);
        }

        void write(const VImage &image, const std::filesystem::path &file, const std::string_view options) {
            const std::string target = file.string() + std::string(options);

            image.write_to_file(target.c_str());
        }

        // Bytes per pixel the format yields on the texture, from a probe.
        double bytes_per_pixel(const Corpus::Spec &spec, const std::filesystem::path &dir) {
            constexpr int PROBE_WIDTH = 1600;
            constexpr int PROBE_HEIGHT = 1200;
            const std::filesystem::path probe = dir / (std::string(".probe-") + std::string(spec.name));

            write(texture(PROBE_WIDTH, PROBE_HEIGHT, spec.sigma), probe, spec.options);

            const auto bytes = static_cast<double>(std::filesystem::file_size(probe));

            std::filesystem::remove(probe);

            return bytes / (static_cast<double>(PROBE_WIDTH) * PROBE_HEIGHT);
        }

        void generate(const Corpus::Spec &spec, const std::filesystem::path &dir) {
            int width = spec.width;
            int height = spec.height;

            if (spec.targetBytes > 0) {
                const double perPixel = bytes_per_pixel(spec, dir);
                const double pixels = std::min(static_cast<double>(spec.targetBytes) / perPixel, static_cast<double>(MAX_PIXELS));

                width = std::min(static_cast<int>(std::sqrt(pixels * 4.0 / 3.0)), MAX_SIDE);
                height = std::min(static_cast<int>(pixels / width), MAX_SIDE);
            }

            const auto started = std::chrono::steady_clock::now();
            const std::filesystem::path file = dir / spec.name;

            write(texture(width, height, spec.sigma), file, spec.options);

            const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;

            std::println(stderr, "corpus: wrote {} ({}x{}, {:.1f} MiB) in {:.1f} s", spec.name, width, height, static_cast<double>(std::filesystem::file_size(file)) / MIB, took.count());
        }
    }

    void Corpus::prepare(const std::filesystem::path &dir) {
        corpusDir = dir;

        std::filesystem::create_directories(dir);

        for (const Spec &spec : SPECS) {
            if (std::filesystem::exists(dir / spec.name)) {
                continue;
            }

            ensure_vips();
            generate(spec, dir);
        }
    }

    const std::filesystem::path &Corpus::dir() {
        return corpusDir;
    }

    std::span<const Corpus::Spec> Corpus::specs() {
        return SPECS;
    }

    std::filesystem::path Corpus::file(const std::string_view name) {
        return corpusDir / name;
    }

    std::vector<std::filesystem::path> Corpus::files() {
        std::vector<std::filesystem::path> held;

        for (const Spec &spec : SPECS) {
            held.push_back(corpusDir / spec.name);
        }

        return held;
    }
}
