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

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Headless.h"
#include "support/Register.h"

#include "image/Decode.h"

int main(int argc, char **argv) {
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
