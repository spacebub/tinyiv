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
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/Orient.h"

namespace bench {
    namespace {
        constexpr int WIDTH = 6656;
        constexpr int HEIGHT = 4992;

        void Orient_apply(benchmark::State &state, const int orientation) {
            for ([[maybe_unused]] auto step : state) {
                state.PauseTiming();
                tiv::Bitmap source = tiv::Bitmap::allocate(WIDTH, HEIGHT);
                std::memset(source.data(), 0x80, source.bytes());
                state.ResumeTiming();

                tiv::Bitmap turned = tiv::Orient::apply(std::move(source), orientation);

                benchmark::DoNotOptimize(turned.data());
            }

            state.SetBytesProcessed(static_cast<std::int64_t>(WIDTH) * HEIGHT * 4 * state.iterations());
        }
    }

    void register_orient() {
        benchmark::RegisterBenchmark("Orient_apply/flip/33MP", Orient_apply, 2)->Unit(benchmark::kMillisecond)->UseRealTime();
        benchmark::RegisterBenchmark("Orient_apply/rotate180/33MP", Orient_apply, 3)->Unit(benchmark::kMillisecond)->UseRealTime();
        benchmark::RegisterBenchmark("Orient_apply/rotate90/33MP", Orient_apply, 6)->Unit(benchmark::kMillisecond)->UseRealTime();
    }
}
