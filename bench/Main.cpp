// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <filesystem>
#include <print>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Headless.h"
#include "support/Register.h"

#include "image/Decode.h"

int main(int argc, char **argv) {
#ifdef __GLIBC__
    // As the app sets it, or peak_rss_mb counts what earlier runs left in the arenas.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread exists yet.
    mallopt(M_MMAP_THRESHOLD, 1024 * 1024);
#endif

    const std::filesystem::path dir = std::filesystem::current_path() / "corpus";

    std::println(stderr, "corpus: {}", dir.string());
    bench::Corpus::prepare(dir);

    bench::register_bitmap();
    bench::register_bmp();
    bench::register_decode();
    bench::register_orient();
    bench::register_pyramid();
    bench::register_upload();
    bench::register_loader();
    bench::register_startup();

    benchmark::Initialize(&argc, argv);

    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    bench::Headless::shutdown();
    tiv::Decode::shutdown();

    return 0;
}
