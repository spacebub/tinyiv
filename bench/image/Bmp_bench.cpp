// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Bmp.h"

namespace bench {
    namespace {
        struct Size {
            const char *name;
            int width;
            int height;
        };

        constexpr std::array<Size, 3> SIZES = {{
                {"4k", 3840, 2160},
                {"33MP", 6656, 4992},
                {"128MP", 13056, 9792},
        }};

        void put16(std::vector<std::uint8_t> &out, const std::size_t at, const std::uint32_t value) {
            out.at(at) = static_cast<std::uint8_t>(value);
            out.at(at + 1) = static_cast<std::uint8_t>(value >> 8);
        }

        void put32(std::vector<std::uint8_t> &out, const std::size_t at, const std::uint32_t value) {
            put16(out, at, value);
            put16(out, at + 2, value >> 16);
        }

        // A bottom up file with an info header and noise for pixels.
        std::vector<std::uint8_t> file(const Size size, const int depth) {
            constexpr std::size_t HEADERS = 14 + 40;
            const std::size_t stride = ((static_cast<std::size_t>(size.width) * static_cast<std::size_t>(depth)) + 31) / 32 * 4;
            std::vector<std::uint8_t> held(HEADERS + (stride * static_cast<std::size_t>(size.height)));
            std::uint32_t seed = 12345;

            held.at(0) = 'B';
            held.at(1) = 'M';
            put32(held, 2, static_cast<std::uint32_t>(held.size()));
            put32(held, 10, HEADERS);
            put32(held, 14, 40);
            put32(held, 18, static_cast<std::uint32_t>(size.width));
            put32(held, 22, static_cast<std::uint32_t>(size.height));
            put16(held, 26, 1);
            put16(held, 28, static_cast<std::uint32_t>(depth));

            for (std::size_t i = HEADERS; i < held.size(); ++i) {
                seed = (seed * 1103515245U) + 12345U;
                held.at(i) = static_cast<std::uint8_t>(seed >> 16);
            }

            return held;
        }

        void Bmp_decode(benchmark::State &state, const Size size, const int depth, const int factor, const tiv::Bmp::Kernel kernel) {
            if (!tiv::Bmp::supports(kernel)) {
                state.SkipWithMessage("kernel unsupported here");

                return;
            }

            const std::vector<std::uint8_t> data = file(size, depth);
            tiv::Bmp::Image image;

            if (!tiv::Bmp::Image::open(data, &image)) {
                state.SkipWithError("open failed");

                return;
            }

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap out;

                if (!image.decode(factor, &out, nullptr, kernel)) {
                    state.SkipWithError("decode failed");

                    break;
                }

                benchmark::DoNotOptimize(out.data());
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(data.size()) * state.iterations());
            state.counters.insert_or_assign("MP/s", benchmark::Counter(static_cast<double>(size.width) * size.height / 1e6, benchmark::Counter::kIsIterationInvariantRate));
        }
    }

    void register_bmp() {
        struct Named {
            const char *name;
            tiv::Bmp::Kernel kernel;
        };

        constexpr std::array<Named, 2> KERNELS = {{
                {"scalar", tiv::Bmp::Kernel::Scalar},
                {"avx2", tiv::Bmp::Kernel::Avx2},
        }};

        for (const Size &size : SIZES) {
            for (const int depth : {24, 32}) {
                for (const Named &kernel : KERNELS) {
                    const std::string suffix = std::string(kernel.name) + "/" + std::to_string(depth) + "bit/" + size.name;

                    benchmark::RegisterBenchmark("Bmp_decode/" + suffix, Bmp_decode, size, depth, 1, kernel.kernel)->Unit(benchmark::kMillisecond)->UseRealTime();
                    benchmark::RegisterBenchmark("Bmp_forced/" + suffix, Bmp_decode, size, depth, 3, kernel.kernel)->Unit(benchmark::kMillisecond)->UseRealTime();
                }
            }
        }
    }
}
