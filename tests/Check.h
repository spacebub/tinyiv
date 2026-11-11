// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_TESTS_CHECK_H
#define TIV_TESTS_CHECK_H


#include <functional>
#include <source_location>
#include <string>
#include <string_view>

namespace test {

struct Case {
    std::string_view name;
    std::function<void()> run;
};

void add(std::string_view name, std::function<void()> run);

// Runs after the last case and before SDL quits, whatever the filter let run.
void at_end(std::function<void()> run);

// Records a failure and goes on, so one run lists every check that failed.
void check(bool passed, std::string_view what, std::source_location where = std::source_location::current());

// For a case that cannot run here, such as one that needs a Vulkan device.
void skip(std::string_view why);

void register_painter();
void register_compare();

}


#endif //TIV_TESTS_CHECK_H
