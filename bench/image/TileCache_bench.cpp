// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Memory.h"
#include "support/Register.h"

#include "image/Bitmap.h"
#include "image/TileCache.h"

namespace bench {
    namespace {
        constexpr std::array<std::string_view, 3> FILES = {"noise.jpg", "noise.png", "noise.tif"};

        std::filesystem::path tile_folder() {
            return Corpus::dir() / "tiles";
        }

        std::shared_ptr<tiv::TileCache> made(const std::filesystem::path &file, const tiv::TileCache::Store store,
                                             std::string *error) {
            return tiv::TileCache::build(file, tile_folder(), {}, store, nullptr, error);
        }

        std::vector<tiv::TileCache::Key> every_tile(const tiv::TileCache &tileCache, const int level) {
            std::vector<tiv::TileCache::Key> keys;
            const tiv::TileCache::Level &at = tileCache.levels()[static_cast<std::size_t>(level)];

            for (int row = 0; row < at.rows; ++row) {
                for (int column = 0; column < at.columns; ++column) {
                    keys.push_back({.level = level, .column = column, .row = row});
                }
            }

            return keys;
        }

        // Asks for the tiles, a queue's worth at a time as the view does, and waits for all of them.
        std::vector<std::shared_ptr<const tiv::Bitmap>> read_all(const tiv::TileCache &tileCache,
                                                                 const std::vector<tiv::TileCache::Key> &keys) {
            struct Signal {
                std::mutex guard;
                std::condition_variable arrived;
            };

            // Shared, as a reader may still be calling the callback after this returns.
            const auto signal = std::make_shared<Signal>();
            std::vector<std::shared_ptr<const tiv::Bitmap>> tiles(keys.size());

            tileCache.on_ready([signal] {
                const std::scoped_lock hold(signal->guard);

                signal->arrived.notify_all();
            });

            std::unique_lock hold(signal->guard);

            for (;;) {
                std::vector<tiv::TileCache::Key> missing;

                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (tiles.at(i) == nullptr) {
                        tiles.at(i) = tileCache.find(keys.at(i));
                    }

                    if (tiles.at(i) == nullptr) {
                        missing.push_back(keys.at(i));
                    }
                }

                if (missing.empty()) {
                    break;
                }

                tileCache.want(missing);
                signal->arrived.wait_for(hold, std::chrono::milliseconds(20));
            }

            tileCache.on_ready({});

            return tiles;
        }

        // Making the pyramid from the file: one pass of decode, halving and compression.
        void TileCache_build(benchmark::State &state, const std::filesystem::path &file,
                             const tiv::TileCache::Store store) {
            std::size_t stored = 0;

            Memory::reset_peak();

            for ([[maybe_unused]] auto step : state) {
                std::string error;
                const std::shared_ptr<tiv::TileCache> tileCache = made(file, store, &error);

                if (tileCache == nullptr) {
                    state.SkipWithError("build failed: " + error);

                    break;
                }

                stored = tileCache->stored_bytes();
            }

            Memory::report_peak(state);
            state.counters["stored_mb"] = static_cast<double>(stored) / 1e6;
        }

        // Unpacking every tile of the finest level, as a pan across the whole image would.
        void TileCache_read(benchmark::State &state, const std::filesystem::path &file,
                            const tiv::TileCache::Store store) {
            std::string error;
            const std::shared_ptr<tiv::TileCache> tileCache = made(file, store, &error);

            if (tileCache == nullptr) {
                state.SkipWithError("build failed: " + error);

                return;
            }

            const std::vector<tiv::TileCache::Key> keys = every_tile(*tileCache, 0);

            for ([[maybe_unused]] auto step : state) {
                tileCache->shrink();
                benchmark::DoNotOptimize(read_all(*tileCache, keys).data());
            }

            state.counters["tiles"] = static_cast<double>(keys.size());
        }

        // The same tiles whichever store holds them, else the numbers above compare nothing.
        void TileCache_stores_agree(benchmark::State &state, const std::filesystem::path &file) {
            std::string error;
            const std::shared_ptr<tiv::TileCache> disk = made(file, tiv::TileCache::Store::Disk, &error);
            const std::shared_ptr<tiv::TileCache> memory = made(file, tiv::TileCache::Store::Memory, &error);

            if (disk == nullptr || memory == nullptr) {
                state.SkipWithError("build failed: " + error);

                return;
            }

            for ([[maybe_unused]] auto step : state) {
                for (std::size_t level = 0; level < disk->levels().size(); ++level) {
                    const std::vector<tiv::TileCache::Key> keys = every_tile(*disk, static_cast<int>(level));
                    const auto fromDisk = read_all(*disk, keys);
                    const auto fromMemory = read_all(*memory, keys);

                    for (std::size_t i = 0; i < keys.size(); ++i) {
                        const std::span<const std::uint8_t> a = fromDisk.at(i)->all();
                        const std::span<const std::uint8_t> b = fromMemory.at(i)->all();

                        if (a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size()) != 0) {
                            state.SkipWithError("tiles differ between the stores");

                            return;
                        }
                    }
                }
            }
        }
    }

    void register_tile_cache() {
        for (const std::string_view name : FILES) {
            const std::filesystem::path file = Corpus::file(name);
            const std::string label(name);

            benchmark::RegisterBenchmark("TileCache_build/disk/" + label, TileCache_build, file,
                                         tiv::TileCache::Store::Disk)
                    ->Unit(benchmark::kMillisecond)
                    ->Iterations(3);
            benchmark::RegisterBenchmark("TileCache_build/memory/" + label, TileCache_build, file,
                                         tiv::TileCache::Store::Memory)
                    ->Unit(benchmark::kMillisecond)
                    ->Iterations(3);
            benchmark::RegisterBenchmark("TileCache_read/disk/" + label, TileCache_read, file,
                                         tiv::TileCache::Store::Disk)
                    ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("TileCache_read/memory/" + label, TileCache_read, file,
                                         tiv::TileCache::Store::Memory)
                    ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("TileCache_stores_agree/" + label, TileCache_stores_agree, file)
                    ->Unit(benchmark::kMillisecond)
                    ->Iterations(1);
        }
    }
}
