// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_SERVICES_LOADER_H
#define TIV_SERVICES_LOADER_H


#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "image/Decode.h"
#include "image/Pyramid.h"

namespace tiv {
    // The threads that decode. Requests coalesce to the latest, the files around it decode
    // in parallel nearest first, whatever navigation has passed is aborted, and the cache
    // stays within its budget: full resolution for the image on screen and its two
    // neighbours, screen sized pyramids for the rest, the farthest evicted first.
    class Loader {

    public:
        enum class Kind : std::uint8_t {
            // A pyramid good for the screen, with the image's info. Possibly the whole image.
            Preview,
            // The whole image, up to the cap.
            Full,
            Failed,
        };

        struct Result {
            std::uint64_t generation = 0;
            Kind kind = Kind::Failed;
            Decode::Info info;
            std::shared_ptr<const Pyramid> pyramid;
            std::string error;
        };

        // Full resolution is capped here, so one image never takes more than this many pixels.
        static constexpr long MAX_PIXELS = 128L * 1000 * 1000;

        // Navigation has to be still this long before a format that already gave a cheap
        // preview is decoded whole.
        static constexpr int REST_MS = 120;

        // The smallest level is the screen divided by this.
        static constexpr int THUMB_DIVISOR = 4;

        static constexpr std::size_t DEFAULT_CACHE_BYTES = std::size_t{1024} * 1024 * 1024;
        static constexpr int MAX_WORKERS = 4;

        // Zero workers means as many as the machine warrants.
        explicit Loader(std::uint32_t eventType, std::size_t cacheBytes = DEFAULT_CACHE_BYTES, int workers = 0);
        ~Loader();

        Loader(const Loader &) = delete;
        Loader(Loader &&) = delete;
        Loader &operator=(const Loader &) = delete;
        Loader &operator=(Loader &&) = delete;

        // Previews are sized to this.
        void set_screen(int width, int height);

        // The image to show, then the ones to have ready either side, nearest first.
        void show(std::uint64_t generation, const std::filesystem::path &current, std::vector<std::filesystem::path> ahead, std::vector<std::filesystem::path> behind);

        // Main thread. False once drained.
        bool take(Result *out);

        // What is decoded and cached for the file, if anything, for warming the GPU ahead of a step.
        bool cached(const std::filesystem::path &file, Decode::Info *info, std::shared_ptr<const Pyramid> *pyramid) const;

        [[nodiscard]] std::size_t cached_bytes() const;
        [[nodiscard]] std::size_t cached_files() const;
        [[nodiscard]] int workers() const { return static_cast<int>(_workers.size()); }

    private:
        struct Request {
            std::uint64_t generation = 0;
            std::uint64_t sequence = 0;
            std::filesystem::path current;
            std::vector<std::filesystem::path> ahead;
            std::vector<std::filesystem::path> behind;
            std::chrono::steady_clock::time_point at;
        };

        // One file. Kept as long as the budget allows.
        struct Entry {
            Decode::Info info;
            std::shared_ptr<const Pyramid> pyramid;
            // The first level is the whole image, up to the cap. Off once trimmed to the screen.
            bool whole = false;
            std::string error;
            // The request that last wanted it.
            std::uint64_t used = 0;
        };

        struct Job {
            std::filesystem::path file;
            bool whole = false;
            std::shared_ptr<Decode::Abort> abort;
            // Bytes the decode is expected to add, once known.
            std::size_t estimate = 0;
        };

        static constexpr int FAR = 1 << 20;

        // Distance in the window, 0 for the current image, FAR outside it.
        [[nodiscard]] int rank(const std::filesystem::path &file) const;
        [[nodiscard]] bool near(const std::filesystem::path &file) const;
        [[nodiscard]] bool running(const std::filesystem::path &file) const;
        [[nodiscard]] bool rested(int ms) const;

        void work();
        bool pick(Job *out, bool *later);
        [[nodiscard]] bool wants_job(const std::filesystem::path &file, bool *whole, bool *later) const;
        void decode(Job &job);
        bool admit(const Job &job, std::size_t estimate);
        void store(const std::filesystem::path &file, Entry entry);
        void abort_strays();
        void trim();
        void cut(Entry &entry);
        bool evict_one();
        void post_cached(const Entry &entry);
        void post(Result result);

        std::uint32_t _event;
        std::size_t _budget;

        mutable std::mutex _guard;
        std::condition_variable _wake;
        bool _running = true;
        Request _request;
        std::deque<Result> _results;

        std::map<std::filesystem::path, Entry> _cache;
        std::size_t _bytes = 0;
        std::vector<Job> _jobs;
        std::size_t _inflight = 0;
        // Set when a prefetch could not be admitted, cleared when memory or the request changes.
        bool _starved = false;
        // Pyramids evicted on the main thread, freed by whichever worker wakes next.
        std::vector<std::shared_ptr<const Pyramid>> _trash;

        int _screenWidth = 3840;
        int _screenHeight = 2160;

        std::vector<std::thread> _workers;
    };
}


#endif //TIV_SERVICES_LOADER_H
