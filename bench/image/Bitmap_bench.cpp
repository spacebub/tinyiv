// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Mapped.h"

namespace bench {
    namespace {
        constexpr int BIG_WIDTH = 13056;
        constexpr int BIG_HEIGHT = 9792;

        // Touching every page is what a decoder does, so the allocation is measured with it.
        void touch(std::uint8_t *data, const std::size_t bytes) {
            for (std::size_t at = 0; at < bytes; at += 4096) {
                data[at] = 1;
            }

            benchmark::DoNotOptimize(data);
        }

        void Bitmap_allocate_touch(benchmark::State &state) {
            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap held = tiv::Bitmap::allocate(BIG_WIDTH, BIG_HEIGHT);

                touch(held.data(), held.bytes());
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(BIG_WIDTH) * BIG_HEIGHT * 4 * state.iterations());
        }

        // The zeroing vector the bitmap used to be.
        void Bitmap_vector_zeroed(benchmark::State &state) {
            for ([[maybe_unused]] auto step : state) {
                std::vector<std::uint8_t> held(static_cast<std::size_t>(BIG_WIDTH) * BIG_HEIGHT * 4);

                touch(held.data(), held.size());
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(BIG_WIDTH) * BIG_HEIGHT * 4 * state.iterations());
        }

        std::size_t checksum(const std::span<const std::uint8_t> data) {
            std::size_t sum = 0;

            for (std::size_t at = 0; at < data.size(); at += 4096) {
                sum += data[at];
            }

            return sum;
        }

        void Mapped_open(benchmark::State &state, const std::filesystem::path file) {
            for ([[maybe_unused]] auto step : state) {
                tiv::Mapped mapped;

                if (!tiv::Mapped::open(file, &mapped)) {
                    state.SkipWithError("cannot map");

                    break;
                }

                benchmark::DoNotOptimize(checksum(mapped.data()));
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(std::filesystem::file_size(file)) * state.iterations());
        }

        void Read_whole(benchmark::State &state, const std::filesystem::path file) {
            for ([[maybe_unused]] auto step : state) {
                std::ifstream in(file, std::ios::binary);
                std::vector<std::uint8_t> data(std::filesystem::file_size(file));

                in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
                benchmark::DoNotOptimize(checksum(data));
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(std::filesystem::file_size(file)) * state.iterations());
        }

        // The byte by byte iterator read the decoders used to start with.
        void Read_istreambuf(benchmark::State &state, const std::filesystem::path file) {
            for ([[maybe_unused]] auto step : state) {
                std::ifstream in(file, std::ios::binary);
                std::vector<std::uint8_t> data;

                data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                benchmark::DoNotOptimize(checksum(data));
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(std::filesystem::file_size(file)) * state.iterations());
        }
    }

    void register_bitmap() {
        benchmark::RegisterBenchmark("Bitmap_allocate_touch/128MP", Bitmap_allocate_touch)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("Bitmap_vector_zeroed/128MP", Bitmap_vector_zeroed)->Unit(benchmark::kMillisecond);

        const std::filesystem::path file = Corpus::file("noise.png");

        benchmark::RegisterBenchmark("Mapped_open/noise.png", Mapped_open, file)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("Read_whole/noise.png", Read_whole, file)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("Read_istreambuf/noise.png", Read_istreambuf, file)->Unit(benchmark::kMillisecond);
    }
}
