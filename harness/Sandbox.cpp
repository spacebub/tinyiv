// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/resource.h>

#include "Sandbox.h"

namespace harness::Sandbox {
    namespace {
        // Marks a folder as made by the harness. fresh() empties only marked folders.
        constexpr std::string_view MARK = ".tiv-harness";

        std::filesystem::path &held() {
            static std::filesystem::path root;

            return root;
        }

        std::filesystem::path &corpus_held() {
            static std::filesystem::path corpus;

            return corpus;
        }

        void mark(const std::filesystem::path &dir) {
            std::filesystem::create_directories(dir);
            std::ofstream(dir / MARK) << "tiv_harness made this folder and empties it on the next run.\n";
        }

        bool marked(const std::filesystem::path &dir) {
            return std::filesystem::is_regular_file(dir / MARK);
        }

        // Set before SDL copies the environment.
        void set(const char *name, const std::filesystem::path &value) {
            std::filesystem::create_directories(value);
            // NOLINTNEXTLINE(concurrency-mt-unsafe): called before any thread starts.
            ::setenv(name, value.c_str(), 1);
        }
    }

    void enter(const std::filesystem::path &root, const std::string_view corpus) {
        std::filesystem::create_directories(root);
        held() = std::filesystem::canonical(root);
        corpus_held() = held() / "corpus" / corpus;

        if (!marked(held())) {
            if (!std::filesystem::is_empty(held())) {
                throw std::runtime_error(held().string() + " is not empty and not a sandbox of tiv_harness");
            }

            mark(held());
        }

        set("HOME", held() / "home");
        set("XDG_CONFIG_HOME", held() / "config");
        set("XDG_CACHE_HOME", held() / "cache");

        // NOLINTNEXTLINE(concurrency-mt-unsafe): called before any thread starts.
        ::setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    }

    const std::filesystem::path &root() {
        return held();
    }

    std::filesystem::path corpus() {
        return corpus_held();
    }

    std::filesystem::path fresh(const std::string_view name) {
        const std::filesystem::path dir = held() / "work" / name;

        if (std::filesystem::exists(dir)) {
            if (!marked(dir) || dir.parent_path().parent_path() != held()) {
                throw std::runtime_error(dir.string() + " was not made by tiv_harness, so it stays as it is");
            }

            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().filename() != MARK) {
                    std::filesystem::remove_all(entry.path());
                }
            }
        }

        mark(dir);

        return dir;
    }

    void copy(const std::filesystem::path &from, const std::filesystem::path &to) {
        std::filesystem::create_directories(to);
        std::filesystem::copy(
                from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    }

    std::filesystem::path config() {
        return held() / "config" / "tinyiv" / "tinyiv.conf";
    }

    void write_config(const std::string_view text) {
        std::filesystem::create_directories(config().parent_path());
        std::ofstream(config(), std::ios::binary | std::ios::trunc) << text;
    }

    std::size_t peak() {
        rusage usage{};

        getrusage(RUSAGE_SELF, &usage);

        // Linux counts kilobytes.
        return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
    }
}
