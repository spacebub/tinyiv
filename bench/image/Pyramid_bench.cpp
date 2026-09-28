// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Pyramid.h"

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

        tiv::Bitmap filled(const int width, const int height) {
            tiv::Bitmap held = tiv::Bitmap::allocate(width, height);
            std::uint32_t seed = 12345;

            for (std::uint8_t &byte : held.all()) {
                seed = (seed * 1103515245U) + 12345U;
                byte = static_cast<std::uint8_t>(seed >> 16);
            }

            return held;
        }

        void Pyramid_halve(benchmark::State &state, const Size size, const tiv::Pyramid::Kernel kernel) {
            if (!tiv::Pyramid::supports(kernel)) {
                state.SkipWithMessage("kernel unsupported here");

                return;
            }

            const tiv::Bitmap source = filled(size.width, size.height);

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap half = tiv::Pyramid::halve(source, kernel);

                benchmark::DoNotOptimize(half.data());
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(source.bytes()) * state.iterations());
            state.counters.insert_or_assign("MP/s", benchmark::Counter(static_cast<double>(source.pixels()) / 1e6, benchmark::Counter::kIsIterationInvariantRate));
        }

        // Every level down to a quarter of a 4k screen, as the loader builds it.
        void Pyramid_build(benchmark::State &state, const Size size) {
            for ([[maybe_unused]] auto step : state) {
                state.PauseTiming();
                tiv::Bitmap base = filled(size.width, size.height);
                state.ResumeTiming();

                const tiv::Pyramid pyramid = tiv::Pyramid::build(std::move(base), 960, 540);

                benchmark::DoNotOptimize(pyramid.levels.data());
            }

            state.counters.insert_or_assign("MP/s", benchmark::Counter(static_cast<double>(size.width) * size.height / 1e6, benchmark::Counter::kIsIterationInvariantRate));
        }
    }

    void register_pyramid() {
        struct Named {
            const char *name;
            tiv::Pyramid::Kernel kernel;
        };

        constexpr std::array<Named, 3> KERNELS = {{
                {"scalar", tiv::Pyramid::Kernel::Scalar},
                {"sse2", tiv::Pyramid::Kernel::Sse2},
                {"avx2", tiv::Pyramid::Kernel::Avx2},
        }};

        for (const Size &size : SIZES) {
            for (const Named &kernel : KERNELS) {
                benchmark::RegisterBenchmark(std::string("Pyramid_halve/") + kernel.name + "/" + size.name, Pyramid_halve, size, kernel.kernel)->Unit(benchmark::kMillisecond)->UseRealTime();
            }

            benchmark::RegisterBenchmark(std::string("Pyramid_build/") + size.name, Pyramid_build, size)->Unit(benchmark::kMillisecond)->UseRealTime();
        }
    }
}
