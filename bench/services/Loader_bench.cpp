// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Memory.h"
#include "support/Register.h"

#include "services/Loader.h"

namespace bench {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr std::uint32_t EVENT = 0x9000;
        constexpr int SCREEN_WIDTH = 3840;
        constexpr int SCREEN_HEIGHT = 2160;
        constexpr int AHEAD = 6;
        constexpr int BEHIND = 2;

        struct Step {
            std::filesystem::path current;
            std::vector<std::filesystem::path> ahead;
            std::vector<std::filesystem::path> behind;
        };

        Step step_at(const std::vector<std::filesystem::path> &files, const std::size_t index, const int direction) {
            Step held;
            const auto count = static_cast<long>(files.size());
            const auto wrap = [&](const long i) { return files[static_cast<std::size_t>(((i % count) + count) % count)]; };

            held.current = files[index];

            for (int i = 1; i <= AHEAD && i < count; ++i) {
                held.ahead.push_back(wrap(static_cast<long>(index) + (i * direction)));
            }

            for (int i = 1; i <= BEHIND && i + AHEAD < count; ++i) {
                held.behind.push_back(wrap(static_cast<long>(index) - (i * direction)));
            }

            return held;
        }

        // Blocks until the loader has something for the generation, or the timeout passes.
        bool wait_for(tiv::Loader &loader, const std::uint64_t generation, tiv::Loader::Result *out, const std::chrono::seconds timeout = std::chrono::seconds(120)) {
            const Clock::time_point until = Clock::now() + timeout;

            while (Clock::now() < until) {
                while (loader.take(out)) {
                    if (out->generation == generation) {
                        return true;
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }

            return false;
        }

        // Settles until nothing has changed for a while, so prefetch has run its course.
        void settle(tiv::Loader &loader) {
            std::size_t last = ~std::size_t{0};
            int still = 0;

            while (still < 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                const std::size_t now = loader.cached_bytes() + loader.cached_files();

                still = now == last ? still + 1 : 0;
                last = now;
            }
        }

        // A fresh loader opening one file: the cold start the app makes.
        void Loader_open(benchmark::State &state, const std::filesystem::path file) {
            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                tiv::Loader loader(EVENT);
                tiv::Loader::Result result;

                loader.set_screen(SCREEN_WIDTH, SCREEN_HEIGHT);
                loader.show(1, file, {}, {});

                if (!wait_for(loader, 1, &result) || result.kind == tiv::Loader::Kind::Failed) {
                    state.SkipWithError("no result: " + result.error);

                    break;
                }

                benchmark::DoNotOptimize(result.pyramid.get());
            }

            Memory::report_peak(state);
        }

        // Stepping between two files both decoded already: what the wheel feels like at rest.
        void Loader_step_cached(benchmark::State &state) {
            const std::vector<std::filesystem::path> files = {Corpus::file("small.jpg"), Corpus::file("small.png")};
            tiv::Loader loader(EVENT);
            tiv::Loader::Result result;
            std::uint64_t generation = 0;

            loader.set_screen(SCREEN_WIDTH, SCREEN_HEIGHT);

            for (const std::filesystem::path &file : files) {
                loader.show(++generation, file, {}, {});

                if (!wait_for(loader, generation, &result)) {
                    state.SkipWithError("no result");

                    return;
                }
            }

            settle(loader);

            // As the app asks in a folder of two, the other file ahead.
            for ([[maybe_unused]] auto step : state) {
                const std::filesystem::path &file = files[generation % 2];
                const std::filesystem::path &other = files[(generation + 1) % 2];

                loader.show(++generation, file, {other}, {});

                if (!wait_for(loader, generation, &result)) {
                    state.SkipWithError("no result");

                    break;
                }
            }
        }

        // A wheel flick over the whole folder, then how long the image it lands on takes, and
        // what the flight through the rest cost in memory.
        void Loader_burst(benchmark::State &state, const std::vector<std::filesystem::path> files, const int gapMs) {
            if (files.size() < 2) {
                state.SkipWithMessage("no folder");

                return;
            }

            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                tiv::Loader loader(EVENT);
                tiv::Loader::Result result;
                std::uint64_t generation = 0;

                loader.set_screen(SCREEN_WIDTH, SCREEN_HEIGHT);

                for (std::size_t i = 0; i < files.size(); ++i) {
                    const Step at = step_at(files, i, 1);

                    loader.show(++generation, at.current, at.ahead, at.behind);
                    std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                }

                const Clock::time_point landed = Clock::now();

                if (!wait_for(loader, generation, &result)) {
                    state.SkipWithError("no result");

                    break;
                }

                state.SetIterationTime(std::chrono::duration<double>(Clock::now() - landed).count());
            }

            state.counters["cache_mb"] = benchmark::Counter(0.0);
            Memory::report_peak(state);
        }

        // Walking a folder at a steady pace with prefetch running: the time each step waits
        // for its first pixels, averaged, and the memory the walk settles at.
        void Loader_walk(benchmark::State &state, const std::vector<std::filesystem::path> files, const int paceMs, const int steps) {
            if (files.size() < 2) {
                state.SkipWithMessage("no folder");

                return;
            }

            Memory::reset_peak();

            tiv::Loader loader(EVENT);
            tiv::Loader::Result result;
            std::uint64_t generation = 0;
            std::size_t index = 0;
            double waited = 0.0;
            double worst = 0.0;
            int counted = 0;

            loader.set_screen(SCREEN_WIDTH, SCREEN_HEIGHT);

            for ([[maybe_unused]] auto step : state) {
                for (int i = 0; i < steps; ++i) {
                    const Step at = step_at(files, index, 1);
                    const Clock::time_point asked = Clock::now();

                    loader.show(++generation, at.current, at.ahead, at.behind);

                    if (!wait_for(loader, generation, &result)) {
                        state.SkipWithError("no result");

                        return;
                    }

                    const double took = std::chrono::duration<double, std::milli>(Clock::now() - asked).count();

                    waited += took;
                    worst = std::max(worst, took);
                    ++counted;
                    index = (index + 1) % files.size();

                    std::this_thread::sleep_until(asked + std::chrono::milliseconds(paceMs));
                }
            }

            state.counters["wait_ms_avg"] = benchmark::Counter(waited / std::max(counted, 1));
            state.counters["wait_ms_max"] = benchmark::Counter(worst);
            state.counters["cache_mb"] = benchmark::Counter(static_cast<double>(loader.cached_bytes()) / (1024.0 * 1024.0));
            state.counters["cache_files"] = benchmark::Counter(static_cast<double>(loader.cached_files()));
            Memory::report_peak(state);
        }
    }

    void register_loader() {
        for (const Corpus::Spec &spec : Corpus::specs()) {
            benchmark::RegisterBenchmark("Loader_open/" + std::string(spec.name), Loader_open, Corpus::file(spec.name))->Unit(benchmark::kMillisecond)->UseRealTime();
        }

        benchmark::RegisterBenchmark("Loader_step_cached", Loader_step_cached)->Unit(benchmark::kMicrosecond)->UseRealTime();

        const std::vector<std::filesystem::path> corpus = Corpus::files();

        benchmark::RegisterBenchmark("Loader_burst/corpus/20ms", Loader_burst, corpus, 20)->Unit(benchmark::kMillisecond)->UseManualTime()->Iterations(3);
        benchmark::RegisterBenchmark("Loader_walk/corpus/500ms", Loader_walk, corpus, 500, 20)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(1);
    }
}
