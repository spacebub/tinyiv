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
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>
#include <glib.h>
#include <vips/vips8>

#include "support/Corpus.h"
#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Mapped.h"
#include "image/decode/JpegBands.h"
#include "image/decode/Support.h"
#include "image/decode/Vips.h"

namespace bench {
    namespace {
        constexpr std::array<std::string_view, 2> FILES = {"restart.jpg", "restart444.jpg"};
        constexpr std::array<unsigned, 4> SCALES = {8, 4, 2, 1};
        constexpr int TILE = 1024;

        // The rows of out against libvips' decode of the whole image, alpha aside. The first
        // mismatch, or empty.
        std::string compare(const tiv::Bitmap &out, const vips::VImage &reference, const int top) {
            std::size_t size = 0;
            auto *pixels = static_cast<std::uint8_t *>(reference.write_to_memory(&size));
            const int width = reference.width();
            std::string held;

            for (int y = 0; y < out.height() && top + y < reference.height() && held.empty(); ++y) {
                const std::span<const std::uint8_t> row = out.row(y);
                const std::uint8_t *expected = pixels + (static_cast<std::size_t>(top + y) * width * 3);

                for (int x = 0; x < width; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        if (row[(static_cast<std::size_t>(x) * 4) + c]
                            != expected[(static_cast<std::size_t>(x) * 3) + c]) {
                            held = std::format("row {} column {} differs", top + y, x);

                            break;
                        }
                    }

                    if (!held.empty()) {
                        break;
                    }
                }
            }

            g_free(pixels);

            return held;
        }

        // Whole images at every scale, and full resolution bands at tile rows, as the libjpeg
        // decode of the whole file gives them. Not a timing: it fails when a band is off by a bit.
        void JpegBands_exact(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Mapped mapped;
            tiv::Decode::JpegBands bands;

            if (!tiv::Mapped::open(file, &mapped) || !tiv::Decode::JpegBands::index(mapped.data(), &bands)) {
                state.SkipWithError("no usable restart markers");

                return;
            }

            tiv::Decode::Vips::ensure();

            for ([[maybe_unused]] auto step : state) {
                for (const unsigned num : SCALES) {
                    const vips::VImage reference = vips::VImage::new_from_file(
                            file.string().c_str(), vips::VImage::option()->set("shrink", static_cast<int>(8 / num)));
                    tiv::Bitmap whole = tiv::Bitmap::allocate(tiv::Decode::JpegBands::scaled(bands.width(), num),
                                                              tiv::Decode::JpegBands::scaled(bands.height(), num));

                    if (!bands.decode(mapped.data(), 0, bands.height(), num, &whole, tiv::Decode::MAX_THREADS,
                                      nullptr)) {
                        state.SkipWithError(std::format("decode at {}/8 failed", num));

                        return;
                    }

                    if (const std::string why = compare(whole, reference, 0); !why.empty()) {
                        state.SkipWithError(std::format("at {}/8: {}", num, why));

                        return;
                    }

                    if (num != 8) {
                        continue;
                    }

                    for (int top = TILE; top < bands.height(); top += 3 * TILE) {
                        tiv::Bitmap band = tiv::Bitmap::allocate(bands.width(), std::min(TILE, bands.height() - top));

                        if (!bands.decode(mapped.data(), top, TILE, 8, &band, tiv::Decode::MAX_THREADS, nullptr)) {
                            state.SkipWithError(std::format("band at {} failed", top));

                            return;
                        }

                        if (const std::string why = compare(band, reference, top); !why.empty()) {
                            state.SkipWithError(std::format("band at {}: {}", top, why));

                            return;
                        }
                    }
                }
            }

            state.counters["step"] = bands.step();
        }

        // Finding the restart markers, a pass over the whole file.
        void JpegBands_index(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Mapped mapped;

            if (!tiv::Mapped::open(file, &mapped)) {
                state.SkipWithError("unreadable");

                return;
            }

            for ([[maybe_unused]] auto step : state) {
                tiv::Decode::JpegBands bands;

                benchmark::DoNotOptimize(tiv::Decode::JpegBands::index(mapped.data(), &bands));
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * mapped.size()));
        }

        // A tile row of full resolution, on one thread and on as many as a decode gets.
        void JpegBands_band(benchmark::State &state, const std::filesystem::path &file, const int threads) {
            tiv::Mapped mapped;
            tiv::Decode::JpegBands bands;

            if (!tiv::Mapped::open(file, &mapped) || !tiv::Decode::JpegBands::index(mapped.data(), &bands)) {
                state.SkipWithError("no usable restart markers");

                return;
            }

            const int top = std::max(0, ((bands.height() / 2) / TILE) * TILE);
            tiv::Bitmap band = tiv::Bitmap::allocate(bands.width(), std::min(TILE, bands.height() - top));

            for ([[maybe_unused]] auto step : state) {
                benchmark::DoNotOptimize(bands.decode(mapped.data(), top, TILE, 8, &band, threads, nullptr));
            }

            state.counters["mp_per_s"] = benchmark::Counter(static_cast<double>(band.pixels()) / 1e6,
                                                            benchmark::Counter::kIsIterationInvariantRate);
        }
    }

    void register_jpeg_bands() {
        for (const std::string_view name : FILES) {
            const std::filesystem::path file = Corpus::file(name);
            const std::string label(name);

            benchmark::RegisterBenchmark("JpegBands_exact/" + label, JpegBands_exact, file)
                    ->Unit(benchmark::kMillisecond)
                    ->Iterations(1);
            benchmark::RegisterBenchmark("JpegBands_index/" + label, JpegBands_index, file)
                    ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("JpegBands_band/1/" + label, JpegBands_band, file, 1)
                    ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("JpegBands_band/max/" + label, JpegBands_band, file, tiv::Decode::MAX_THREADS)
                    ->Unit(benchmark::kMillisecond);
        }
    }
}
