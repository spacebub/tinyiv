// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Memory.h"
#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "services/Loader.h"

namespace bench {
    namespace {
        constexpr int SCREEN_WIDTH = 3840;
        constexpr int SCREEN_HEIGHT = 2160;

        // The box the app uses for an image over the cap.
        std::pair<int, int> capped(const tiv::Decode::Info &info) {
            if (info.pixels() <= tiv::Loader::max_pixels()) {
                return {info.width, info.height};
            }

            const double shrink = std::sqrt(static_cast<double>(tiv::Loader::max_pixels()) / static_cast<double>(info.pixels()));

            return {static_cast<int>(info.width * shrink), static_cast<int>(info.height * shrink)};
        }

        void report(benchmark::State &state, const std::filesystem::path &file, const std::int64_t pixels) {
            state.SetBytesProcessed(static_cast<std::int64_t>(std::filesystem::file_size(file)) * state.iterations());
            state.counters.insert_or_assign("MP/s", benchmark::Counter(static_cast<double>(pixels) / 1e6, benchmark::Counter::kIsIterationInvariantRate));
            Memory::report_peak(state);
        }

        void Decode_probe(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Decode::Info info;

            for ([[maybe_unused]] auto step : state) {
                if (!tiv::Decode::probe(file, &info)) {
                    state.SkipWithError("probe failed");

                    break;
                }

                benchmark::DoNotOptimize(info);
            }
        }

        // The whole image, as the loader decodes a format without a cheap preview.
        void Decode_full(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Decode::Info info;

            if (!tiv::Decode::probe(file, &info)) {
                state.SkipWithError("probe failed");

                return;
            }

            const auto [boxWidth, boxHeight] = capped(info);
            const tiv::Decode::Fit fit = info.pixels() > tiv::Loader::max_pixels() ? tiv::Decode::Fit::Force : tiv::Decode::Fit::Cheap;

            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap out;
                std::string error;

                if (!tiv::Decode::load(file, boxWidth, boxHeight, &out, &error, nullptr, fit)) {
                    state.SkipWithError(error);

                    break;
                }

                benchmark::DoNotOptimize(out.data());
            }

            report(state, file, info.pixels());
        }

        // What the loader asks for first: something the screen holds. Over the cap, the loader
        // never lets a decoder hold the image whole, so neither does this.
        void Decode_preview(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Decode::Info info;

            if (!tiv::Decode::probe(file, &info)) {
                state.SkipWithError("probe failed");

                return;
            }

            const bool over = info.pixels() > tiv::Loader::max_pixels();
            const auto [boxWidth, boxHeight] = over ? capped(info) : std::pair{SCREEN_WIDTH, SCREEN_HEIGHT};
            const tiv::Decode::Fit fit = over ? tiv::Decode::Fit::Force : tiv::Decode::Fit::Cheap;

            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap out;
                std::string error;

                if (!tiv::Decode::load(file, boxWidth, boxHeight, &out, &error, nullptr, fit)) {
                    state.SkipWithError(error);

                    break;
                }

                benchmark::DoNotOptimize(out.data());
            }

            report(state, file, info.pixels());
        }

        // Forced within the screen, the path for images too large to hold whole.
        void Decode_forced(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Decode::Info info;

            if (!tiv::Decode::probe(file, &info)) {
                state.SkipWithError("probe failed");

                return;
            }

            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap out;
                std::string error;

                if (!tiv::Decode::load(file, SCREEN_WIDTH, SCREEN_HEIGHT, &out, &error, nullptr, tiv::Decode::Fit::Force)) {
                    state.SkipWithError(error);

                    break;
                }

                benchmark::DoNotOptimize(out.data());
            }

            report(state, file, info.pixels());
        }

        // A decode cut short after a few rows, which is what a wheel flick costs per image passed.
        void Decode_aborted(benchmark::State &state, const std::filesystem::path &file) {
            tiv::Decode::Info info;

            if (!tiv::Decode::probe(file, &info)) {
                state.SkipWithError("probe failed");

                return;
            }

            for ([[maybe_unused]] auto step : state) {
                tiv::Bitmap out;
                tiv::Decode::Abort abort;

                abort.request();
                benchmark::DoNotOptimize(tiv::Decode::load(file, SCREEN_WIDTH, SCREEN_HEIGHT, &out, nullptr, &abort));
            }
        }
    }

    void register_decode() {
        for (const Corpus::Spec &spec : Corpus::specs()) {
            const std::filesystem::path file = Corpus::file(spec.name);
            const std::string name(spec.name);

            benchmark::RegisterBenchmark("Decode_probe/" + name, Decode_probe, file)->Unit(benchmark::kMicrosecond);
            benchmark::RegisterBenchmark("Decode_full/" + name, Decode_full, file)->Unit(benchmark::kMillisecond)->UseRealTime();
            benchmark::RegisterBenchmark("Decode_preview/" + name, Decode_preview, file)->Unit(benchmark::kMillisecond)->UseRealTime();
            benchmark::RegisterBenchmark("Decode_forced/" + name, Decode_forced, file)->Unit(benchmark::kMillisecond)->UseRealTime();
            benchmark::RegisterBenchmark("Decode_aborted/" + name, Decode_aborted, file)->Unit(benchmark::kMicrosecond);
        }
    }
}
