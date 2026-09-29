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


#include <atomic>
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

#include "image/Animation.h"
#include "image/Pyramid.h"
#include "image/TileCache.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"

namespace tiv {
    // Requests coalesce to the latest, the files around it decode in parallel nearest first, and whatever
    // navigation has passed is aborted. The cache keeps full resolution for the image on screen and its two
    // neighbours, screen sized pyramids for the rest and a few just passed, the farthest evicted first.
    class Loader {

    public:
        enum class Kind : std::uint8_t {
            // A pyramid good for the screen, with the image's info. Possibly the whole image.
            Preview,
            // The whole image, up to the cap.
            Full,
            // An image too large for memory, or asked to stream, whose tiles are being made before
            // it can show. See progress().
            Building,
            Failed,
        };

        struct Result {
            std::uint64_t generation = 0;
            Kind kind = Kind::Failed;
            Decode::Info info;
            std::shared_ptr<const Pyramid> pyramid;
            std::string error;
            // Failed because nothing here reads the format.
            bool unsupported = false;
            // Every frame, once decoded, for an image whose info counts more than one.
            std::shared_ptr<const Animation> animation;
            // Instead of the pyramid, for an image shown from tiles.
            std::shared_ptr<const TileCache> tileCache;
            // Where tiles being made go on disk. Empty when they stay in memory.
            std::filesystem::path tileFolder;
        };

        // Full resolution is capped where the image's pyramid would take more than half the
        // machine's memory, and never below this. It is also what an image falls back to when
        // the system has too little left for the whole of it.
        static constexpr std::int64_t MIN_PIXELS = std::int64_t{128} * 1000 * 1000;

        // The most pixels one image decodes to. An image beyond it streams down to fit.
        [[nodiscard]] static std::int64_t max_pixels();

        // Navigation has to be still this long before a format that already gave a cheap
        // preview is decoded whole.
        static constexpr int REST_MS = 120;

        // Navigation has to be still this long before freed memory goes back to the system,
        // since until then the next image reuses it without the system zeroing fresh pages.
        static constexpr int GIVE_BACK_MS = 1000;

        // The frames of one animation take at most this, shrunk to fit.
        static constexpr std::size_t MAX_ANIMATION_BYTES = std::size_t{384} * 1024 * 1024;

        // The smallest level is the screen divided by this.
        static constexpr int THUMB_DIVISOR = 4;

        static constexpr std::size_t DEFAULT_CACHE_BYTES = std::size_t{2048} * 1024 * 1024;
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

        // Where pyramids on disk go, as TileCache::location() takes it. Said before the first show().
        void set_tile_folder(std::filesystem::path folder);

        // HDR images decode for this display, and wait until it has been said, so nothing is
        // made for the wrong one. What is decoded of them for another goes, and so does
        // whatever such a decode still running makes.
        void set_display(const Tone::Display &display);

        // The image to show, then the ones to have ready either side, nearest first.
        void show(std::uint64_t generation, const std::filesystem::path &current,
                  std::vector<std::filesystem::path> ahead, std::vector<std::filesystem::path> behind);

        // Main thread. False once drained.
        bool take(Result *out);

        // What is decoded and cached for the file, if anything, for warming the GPU ahead of a step.
        bool cached(const std::filesystem::path &file, Decode::Info *info,
                    std::shared_ptr<const Pyramid> *pyramid) const;

        // The file changed on disk, so what is decoded of it goes and decodes of it stop.
        void forget(const std::filesystem::path &file);

        // The file now says this orientation. Decodes of it still running read the old one, so they stop.
        void reoriented(const std::filesystem::path &file, int orientation);

        // No decode is running.
        [[nodiscard]] bool idle() const;

        // S: streaming mode, where every still image shows from a pyramid of tiles, not only
        // those too large for memory. Only those go on disk, the rest stay in memory. Everything
        // decoded goes, so the next show() decodes the new way.
        void stream_all(bool on);
        [[nodiscard]] bool streaming_all() const;

        // How far the tiles being made for the current image are, from 0 to 1.
        [[nodiscard]] float progress() const { return _progress.load(std::memory_order_relaxed); }

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
            std::shared_ptr<const Animation> animation;
            // Shown from tiles. The tile cache is null until one is made, which only the current image does.
            bool streamed = false;
            std::shared_ptr<const TileCache> tileCache;
            std::string error;
            bool unsupported = false;
            // The request that last wanted it.
            std::uint64_t used = 0;
            // What an HDR image was decoded for.
            Tone::Display display;
        };

        struct Job {
            std::filesystem::path file;
            bool whole = false;
            // Every frame of an animation, rather than the first.
            bool frames = false;
            std::shared_ptr<Decode::Abort> abort;
            // Bytes the decode is expected to add, once known.
            std::size_t estimate = 0;
            // Writing tiles to disk, which stops once the file is no longer the current one.
            bool streaming = false;
            // What the image is, once streaming, for showing it again while the tiles are written.
            Decode::Info info;
            std::filesystem::path tileFolder;
        };

        static constexpr int FAR = 1U << 20U;

        // Distance in the window, 0 for the current image, FAR outside it.
        [[nodiscard]] int rank(const std::filesystem::path &file) const;
        [[nodiscard]] bool near(const std::filesystem::path &file) const;
        [[nodiscard]] bool running(const std::filesystem::path &file) const;
        [[nodiscard]] bool rested(int ms) const;
        // Under the lock. Only an HDR image decodes for the display.
        [[nodiscard]] Tone::Display display_for(const Decode::Info &info) const;

        void work();
        bool pick(Job *out, bool *later);
        [[nodiscard]] bool wants_job(const std::filesystem::path &file, bool *whole, bool *frames, bool *later) const;
        void decode(Job &job);
        void decode_frames(Job &job);
        void decode_stream(Job &job, Entry entry);
        bool admit(const Job &job, std::size_t estimate);
        bool finish(const std::filesystem::path &file);
        void store(const std::filesystem::path &file, Entry entry);
        void insert(const std::filesystem::path &file, Entry entry);
        // A null animation means the frames failed, and the image stays still.
        void store_frames(const std::filesystem::path &file, std::shared_ptr<const Animation> animation);
        void release(Entry &entry);
        void release_animation(Entry &entry);
        void abort_strays();
        void trim();
        void evict_outside();
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
        // Pyramids and animations evicted on the main thread, freed by whichever worker wakes next.
        std::vector<std::shared_ptr<const void>> _trash;
        // Memory was freed since it was last given back.
        bool _freed = false;

        bool _streamAll = false;
        std::filesystem::path _tileFolder;
        std::atomic<float> _progress = 0.0F;

        int _screenWidth = 3840;
        int _screenHeight = 2160;
        Tone::Display _display;
        bool _displayKnown = false;

        std::vector<std::thread> _workers;
    };
}


#endif //TIV_SERVICES_LOADER_H
