// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <functional>
#include <print>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "Check.h"

namespace test {
namespace {
struct Run {
    std::vector<Case> cases;
    std::vector<std::function<void()>> ends;
    std::string_view current;
    int failures = 0;
    bool skipped = false;
};

Run &run() {
    static Run held;

    return held;
}
}

void add(const std::string_view name, std::function<void()> run) {
    test::run().cases.push_back({.name = name, .run = std::move(run)});
}

void at_end(std::function<void()> run) {
    test::run().ends.push_back(std::move(run));
}

void check(const bool passed, const std::string_view what, const std::source_location where) {
    if (passed) {
        return;
    }

    ++run().failures;
    std::println(stderr, "FAIL {}: {} ({}:{})", run().current, what, where.file_name(), where.line());
}

void skip(const std::string_view why) {
    run().skipped = true;
    std::println(stderr, "SKIP {}: {}", run().current, why);
}
}

// NOLINTNEXTLINE(bugprone-exception-escape): std::println throws only when the stream fails.
int main(const int argc, char **argv) {
    const std::span args(argv, static_cast<std::size_t>(argc));
    const std::string_view only = args.size() > 1 ? args[1] : "";

    // The offscreen driver needs no display, and its Vulkan surface is headless.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init: {}", SDL_GetError());

        return 1;
    }

    test::register_painter();
    test::register_compare();
    test::register_tone();

    int ran = 0;

    for (const test::Case &each : test::run().cases) {
        if (!only.empty() && !each.name.contains(only)) {
            continue;
        }

        const int before = test::run().failures;

        test::run().current = each.name;
        test::run().skipped = false;
        each.run();
        ++ran;

        if (test::run().failures == before && !test::run().skipped) {
            std::println("ok   {}", each.name);
        }
    }

    for (const std::function<void()> &end : test::run().ends) {
        end();
    }

    SDL_Quit();
    std::println("{} cases, {} failed checks", ran, test::run().failures);

    return test::run().failures == 0 ? 0 : 1;
}
