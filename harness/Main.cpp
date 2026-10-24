// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "Corpus.h"
#include "Driver.h"
#include "Sandbox.h"
#include "Sittings.h"
#include "image/decode/Decode.h"

namespace {
    using Clock = std::chrono::steady_clock;

    struct Options {
        std::filesystem::path sandbox = std::filesystem::current_path() / "harness-sandbox";
        harness::Size size = harness::Size::Small;
        std::vector<std::string> sittings;
        double patience = 30.0;
        int repeat = 1;
        // The file for the start and end of each sitting, on the clock of perf record -k CLOCK_MONOTONIC.
        std::filesystem::path marks;
        bool corpusOnly = false;
        bool list = false;
    };

    void usage() {
        std::println(stderr,
                     "Usage: tiv_harness [--sandbox DIR] [--size small|full] [--sitting NAME]... [--repeat N]\n"
                     "                   [--patience SECONDS] [--marks FILE] [--corpus] [--list]\n"
                     "Runs tinyiv offscreen through scripted sittings over a corpus it writes itself.\n"
                     "Everything goes in the sandbox. The default is harness-sandbox in the working directory.\n"
                     "--corpus writes and checks the corpus alone. --marks writes when each sitting ran,\n"
                     "for perf report --time. Exits 1 when a check failed.");
    }

    template <typename T>
    bool number(const std::string_view text, T &out) {
        const char *last = std::to_address(text.end());
        const auto [end, failure] = std::from_chars(std::to_address(text.begin()), last, out);

        return failure == std::errc() && end == last;
    }

    // An option that takes a value. False when it is not one or the value does not fit it.
    bool take(Options &options, const std::string_view option, const std::string_view value) {
        if (option == "--sandbox") {
            options.sandbox = value;
        } else if (option == "--size" && (value == "small" || value == "full")) {
            options.size = value == "full" ? harness::Size::Full : harness::Size::Small;
        } else if (option == "--sitting") {
            options.sittings.emplace_back(value);
        } else if (option == "--repeat") {
            return number(value, options.repeat) && options.repeat > 0;
        } else if (option == "--patience") {
            return number(value, options.patience) && options.patience > 0.0;
        } else if (option == "--marks") {
            options.marks = value;
        } else {
            return false;
        }

        return true;
    }

    // False when the arguments were not understood.
    bool parse(const std::span<char *> arguments, Options &options) {
        for (std::size_t at = 1; at < arguments.size(); ++at) {
            const std::string_view argument = arguments[at];

            if (argument == "--corpus") {
                options.corpusOnly = true;
            } else if (argument == "--list") {
                options.list = true;
            } else {
                ++at;

                if (at >= arguments.size() || !take(options, argument, arguments[at])) {
                    return false;
                }
            }
        }

        return true;
    }

    double monotonic() {
        timespec now{};

        clock_gettime(CLOCK_MONOTONIC, &now);

        return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) / 1e9);
    }

    // Prints the steps of a sitting summed by name, slowest first.
    void print_steps(const harness::Report &report) {
        struct Sum {
            int count = 0;
            double total = 0.0;
            double longest = 0.0;
        };

        std::map<std::string, Sum> sums;

        for (const harness::Report::Step &step : report.steps) {
            Sum &sum = sums.try_emplace(step.name).first->second;

            ++sum.count;
            sum.total += step.seconds;
            sum.longest = std::max(sum.longest, step.seconds);
        }

        std::vector<std::pair<std::string, Sum>> sorted(sums.begin(), sums.end());

        std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.second.total > b.second.total; });

        for (const auto &[name, sum] : sorted) {
            std::println("    {:<18} {:>4} x  {:>8.3f} s  longest {:.3f} s", name, sum.count, sum.total, sum.longest);
        }
    }

    bool chosen(const Options &options, const std::string_view name) {
        return options.sittings.empty() || std::ranges::find(options.sittings, name) != options.sittings.end();
    }

    // Runs the sittings chosen, prints what each took and adds what failed.
    void drive(const Options &options, const harness::Library &library, std::vector<std::string> &failed) {
        std::ofstream marks;

        if (!options.marks.empty()) {
            marks.open(options.marks, std::ios::trunc);
        }

        std::vector<std::pair<std::string, double>> times;
        const Clock::time_point driving = Clock::now();

        for (int round = 0; round < options.repeat; ++round) {
            for (const harness::Sitting &sitting : harness::sittings()) {
                if (!chosen(options, sitting.name)) {
                    continue;
                }

                std::println("{}: {}", sitting.name, sitting.about);

                harness::Report report;
                harness::Driver driver(report, options.patience);
                const double start = monotonic();
                const Clock::time_point started = Clock::now();

                sitting.run(library, driver);

                const double seconds = std::chrono::duration<double>(Clock::now() - started).count();

                if (marks.is_open()) {
                    std::println(marks, "{}\t{:.6f}\t{:.6f}", sitting.name, start, monotonic());
                }

                times.emplace_back(sitting.name, seconds);
                std::println("  {:.2f} s, {} steps, {} failed", seconds, report.steps.size(), report.failed.size());
                print_steps(report);

                for (const std::string &what : report.failed) {
                    failed.push_back(std::string(sitting.name) + ": " + what);
                }
            }
        }

        std::println("\ntotals");

        for (const auto &[name, seconds] : times) {
            std::println("  {:<12} {:>8.2f} s", name, seconds);
        }

        std::println("  {:<12} {:>8.2f} s, peak {:.0f} MB resident", "all",
                     std::chrono::duration<double>(Clock::now() - driving).count(),
                     static_cast<double>(harness::Sandbox::peak()) / 1e6);
    }

    int run(const std::span<char *> arguments) {
        Options options;

        if (!parse(arguments, options)) {
            usage();

            return 2;
        }

        if (options.list) {
            for (const harness::Sitting &sitting : harness::sittings()) {
                std::println("{:<12} {}", sitting.name, sitting.about);
            }

            return 0;
        }

        for (const std::string &name : options.sittings) {
            if (std::ranges::find(harness::sittings(), name, &harness::Sitting::name) == harness::sittings().end()) {
                std::println(stderr, "tiv_harness: no sitting is called {}, see --list", name);

                return 2;
            }
        }

        // The output often goes through a pipe and is read as it arrives.
        std::ignore = std::setvbuf(stdout, nullptr, _IOLBF, 0);

        harness::Sandbox::enter(options.sandbox, options.size == harness::Size::Full ? "full" : "small");
        std::println("sandbox {}", harness::Sandbox::root().string());

        std::vector<std::string> failed;
        const Clock::time_point began = Clock::now();
        const harness::Library library(harness::prepare_corpus(harness::Sandbox::corpus(), options.size, &failed));

        std::println("corpus ready in {:.1f} s, peak {:.0f} MB resident",
                     std::chrono::duration<double>(Clock::now() - began).count(),
                     static_cast<double>(harness::Sandbox::peak()) / 1e6);

        for (const std::string &what : failed) {
            std::println(stderr, "  corpus: {}", what);
        }

        // A file that probes wrong is a finding about tinyiv. Only a corpus that could not be written stops the run.
        if (options.corpusOnly
            || std::ranges::any_of(failed, [](const std::string &what) { return what.contains(" was not written"); })) {
            tiv::Decode::shutdown();

            return failed.empty() ? 0 : 1;
        }

        for (std::string &what : failed) {
            what.insert(0, "corpus: ");
        }

        drive(options, library, failed);

        if (!failed.empty()) {
            std::println(stderr, "\n{} checks failed:", failed.size());

            for (const std::string &what : failed) {
                std::println(stderr, "  {}", what);
            }
        }

        tiv::Decode::shutdown();

        return failed.empty() ? 0 : 1;
    }
}

int main(int argc, char *argv[]) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception &trouble) {
        // NOLINTNEXTLINE(cert-err33-c,modernize-use-std-print,cppcoreguidelines-pro-type-vararg): std::println may be what threw.
        std::fprintf(stderr, "tiv_harness stopped: %s\n", trouble.what());
    }

    return 1;
}
