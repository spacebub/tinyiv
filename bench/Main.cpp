// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <tuple>

#include <benchmark/benchmark.h>

#include "support/Corpus.h"
#include "support/Headless.h"
#include "support/Register.h"

#include "image/decode/Decode.h"

namespace {
    int run(int argc, char **argv) {
        const std::filesystem::path dir = std::filesystem::current_path() / "corpus";

        std::println(stderr, "corpus: {}", dir.string());
        bench::Corpus::prepare(dir);

        bench::register_bitmap();
        bench::register_bmp();
        bench::register_jpeg_bands();
        bench::register_decode();
        bench::register_pyramid();
        bench::register_tile_cache();
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
}

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &failure) {
        std::ignore = std::fputs("tiv_bench: ", stderr);
        std::ignore = std::fputs(failure.what(), stderr);
        std::ignore = std::fputc('\n', stderr);

        return 1;
    }
}
