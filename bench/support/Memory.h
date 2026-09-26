// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_BENCH_SUPPORT_MEMORY_H
#define TIV_BENCH_SUPPORT_MEMORY_H


#include <cstddef>

#include <benchmark/benchmark.h>

namespace bench::Memory {

    // Resident bytes now, and the most since the last reset.
    [[nodiscard]] std::size_t resident();
    [[nodiscard]] std::size_t peak();
    void reset_peak();

    // Adds peak_rss_mb, the most resident since reset_peak(), and rss_added_mb, how far above
    // what was resident at the reset that went.
    void report_peak(benchmark::State &state);

}


#endif //TIV_BENCH_SUPPORT_MEMORY_H
