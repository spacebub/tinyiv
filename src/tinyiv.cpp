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
#include <span>
#include <string>
#include <tuple>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "app/App.h"
#include "image/Decode.h"

namespace {
    int run(const std::span<char *> args) {
        if (args.size() > 2) {
            std::println(stderr, "usage: tinyiv [image]");

            return 2;
        }

        int status = 0;

        {
            tiv::App app;
            std::string error;

            if (app.start(args.size() == 2 ? args.back() : std::filesystem::path(), &error)) {
                app.run();
            } else {
                std::println(stderr, "tinyiv: {}", error);
                status = 1;
            }
        }

        tiv::Decode::shutdown();

        return status;
    }
}

int main(const int argc, char **argv) {
#ifdef __GLIBC__
    // glibc raises its mmap threshold with each large free, after which freed pixel buffers stay resident in its arenas.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread exists yet.
    mallopt(M_MMAP_THRESHOLD, 1024 * 1024);
#endif

    try {
        return run({argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception &failure) {
        std::ignore = std::fputs("tinyiv: ", stderr);
        std::ignore = std::fputs(failure.what(), stderr);
        std::ignore = std::fputc('\n', stderr);

        return 1;
    }
}
