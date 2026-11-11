// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <benchmark/benchmark.h>

#include "support/Memory.h"

namespace bench {
namespace {
std::size_t status_kb(const std::string &key) {
    std::ifstream in("/proc/self/status");
    std::string line;

    while (std::getline(in, line)) {
        if (line.starts_with(key)) {
            return std::stoull(line.substr(key.size() + 1)) * 1024;
        }
    }

    return 0;
}
}

std::size_t Memory::resident() {
    return status_kb("VmRSS:");
}

std::size_t Memory::peak() {
    return status_kb("VmHWM:");
}

namespace {
std::size_t &baseline() {
    static std::size_t held = 0;

    return held;
}
}

void Memory::reset_peak() {
    // Writing 5 resets the high water mark, which needs no privilege for one's own process.
    // https://man7.org/linux/man-pages/man5/proc_pid_clear_refs.5.html
    std::ofstream("/proc/self/clear_refs") << "5\n";
    baseline() = resident();
}

void Memory::report_peak(benchmark::State &state) {
    const std::size_t most = peak();

    state.counters.insert_or_assign("peak_rss_mb", benchmark::Counter(static_cast<double>(most) / (1024.0 * 1024.0)));
    state.counters.insert_or_assign(
            "rss_added_mb",
            benchmark::Counter(static_cast<double>(most > baseline() ? most - baseline() : 0) / (1024.0 * 1024.0)));
}

namespace {
unsigned shift_of(const std::string &unit) {
    if (unit == "GiB") {
        return 30U;
    }

    if (unit == "MiB") {
        return 20U;
    }

    return unit == "KiB" ? 10U : 0U;
}
}

// Each open DRM file reports its client, and two files of one client report the same memory.
std::size_t Memory::vram() {
    std::set<std::string> clients;
    std::size_t total = 0;
    std::error_code error;

    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fdinfo", error)) {
        std::ifstream file(entry.path());
        std::string line;
        std::string client;
        std::size_t bytes = 0;

        while (std::getline(file, line)) {
            std::istringstream words(line);
            std::string key;

            words >> key;

            if (key == "drm-client-id:") {
                words >> client;
            } else if (key == "drm-resident-vram:") {
                std::size_t amount = 0;
                std::string unit;

                words >> amount >> unit;
                bytes = amount << shift_of(unit);
            }
        }

        if (!client.empty() && clients.insert(client).second) {
            total += bytes;
        }
    }

    return total;
}
}
