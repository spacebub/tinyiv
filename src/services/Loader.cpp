// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Animation.h"
#include "image/Bitmap.h"
#include "image/Orient.h"
#include "image/Pyramid.h"
#include "image/TileCache.h"
#include "image/decode/Decode.h"
#include "services/Loader.h"
#include "services/Memory.h"

namespace tiv {
    namespace {
        int default_workers() {
            const int cores = static_cast<int>(std::thread::hardware_concurrency());

            return std::clamp(cores / 8, 2, Loader::MAX_WORKERS);
        }

        struct Box {
            int width = 0;
            int height = 0;
        };

        Box capped(const Decode::Info &info, const std::int64_t limit = Loader::max_pixels()) {
            if (info.pixels() <= limit) {
                return {.width = info.width, .height = info.height};
            }

            const double shrink = std::sqrt(static_cast<double>(limit) / static_cast<double>(info.pixels()));

            return {
                    .width = std::max(static_cast<int>(std::floor(info.width * shrink)), 1),
                    .height = std::max(static_cast<int>(std::floor(info.height * shrink)), 1),
            };
        }

        Box fitted(const Decode::Info &info, const int boxWidth, const int boxHeight) {
            if (info.width <= boxWidth && info.height <= boxHeight) {
                return {.width = info.width, .height = info.height};
            }

            const double shrink =
                    std::min(static_cast<double>(boxWidth) / info.width, static_cast<double>(boxHeight) / info.height);

            return {
                    .width = std::max(static_cast<int>(std::floor(info.width * shrink)), 1),
                    .height = std::max(static_cast<int>(std::floor(info.height * shrink)), 1),
            };
        }

        // Fits the box, enlarged if need be.
        Box filled(const Decode::Info &info, const int boxWidth, const int boxHeight) {
            const double scale =
                    std::min(static_cast<double>(boxWidth) / info.width, static_cast<double>(boxHeight) / info.height);

            return {
                    .width = std::max(static_cast<int>(std::floor(info.width * scale)), 1),
                    .height = std::max(static_cast<int>(std::floor(info.height * scale)), 1),
            };
        }

        void take_size(const Bitmap &decoded, Decode::Info *info) {
            const bool swapped = Orient::swaps(info->orientation);

            info->width = swapped ? decoded.height() : decoded.width();
            info->height = swapped ? decoded.width() : decoded.height();
        }

        // A pyramid is its base and a third again.
        std::size_t pyramid_bytes(const Box box) {
            return static_cast<std::size_t>(box.width) * static_cast<std::size_t>(box.height) * Bitmap::CHANNELS * 4
                   / 3;
        }

        struct Plan {
            Box box;
            Decode::Fit fit = Decode::Fit::Cheap;
            bool whole = true;
            std::size_t estimate = 0;
        };

        Plan plan(const Decode::Info &info, const bool wantsWhole, const bool over, const std::size_t budget,
                  const int screenWidth, const int screenHeight) {
            const bool scalable = Decode::scalable(info.kind);
            const std::size_t nativeBytes = static_cast<std::size_t>(info.pixels()) * Bitmap::CHANNELS;
            // WebP and JXL have no cheaper decode than the native size, which is halved down after,
            // as long as the transient bitmap fits the budget.
            const bool nativeThenHalve = over && (info.kind == Decode::Format::WebP || info.kind == Decode::Format::Jxl)
                                         && nativeBytes <= budget;

            Plan planned;

            if (scalable) {
                planned.box = filled(info, screenWidth, screenHeight);
            } else if (!wantsWhole && Decode::scales_cheaply(info.kind)) {
                planned.box = fitted(info, screenWidth, screenHeight);
                planned.whole = false;
            } else if (over && !nativeThenHalve) {
                planned.box = capped(info);
                planned.fit = Decode::Fit::Force;
            } else {
                planned.box = capped(info);
            }

            planned.estimate = pyramid_bytes(planned.whole && !scalable ? capped(info) : planned.box);

            if (planned.whole && nativeThenHalve) {
                planned.estimate += nativeBytes;
            }

            return planned;
        }
    }

    std::int64_t Loader::max_pixels() {
        static const std::int64_t held = [] {
            // SDL counts whole mebibytes: https://wiki.libsdl.org/SDL3/SDL_GetSystemRAM
            const std::int64_t half = static_cast<std::int64_t>(SDL_GetSystemRAM()) * 1024 * 1024 / 2;

            // A pyramid is four bytes a pixel and a third again.
            return std::max(half * 3 / (Bitmap::CHANNELS * std::int64_t{4}), MIN_PIXELS);
        }();

        return held;
    }

    Loader::Loader(const std::uint32_t eventType, const std::size_t cacheBytes, const int workers)
        : _event(eventType), _budget(cacheBytes) {
        const int count = workers > 0 ? workers : default_workers();

        _workers.reserve(static_cast<std::size_t>(count));

        for (int i = 0; i < count; ++i) {
            _workers.emplace_back([this] { work(); });
        }
    }

    Loader::~Loader() {
        {
            const std::scoped_lock hold(_guard);

            _running = false;

            for (const Job &job : _jobs) {
                job.abort->request();
            }
        }

        _wake.notify_all();

        for (std::thread &worker : _workers) {
            worker.join();
        }
    }

    void Loader::set_screen(const int width, const int height) {
        const std::scoped_lock hold(_guard);

        _screenWidth = std::max(width, 1);
        _screenHeight = std::max(height, 1);
    }

    void Loader::set_tiles(std::filesystem::path folder, const bool small, const bool persist) {
        const std::scoped_lock hold(_guard);

        _tileFolder = std::move(folder);
        _smallTiles = small;
        _persistTiles = persist;
    }

    Tone::Display Loader::display_for(const Decode::Info &info) const {
        return info.hdr ? _display : Tone::Display{};
    }

    void Loader::set_display(const Tone::Display &display) {
        {
            const std::scoped_lock hold(_guard);

            if (_displayKnown && _display.headroom == display.headroom) {
                return;
            }

            _displayKnown = true;
            _display = display;

            for (auto found = _cache.begin(); found != _cache.end();) {
                if (found->second.info.hdr) {
                    release(found->second);
                    found = _cache.erase(found);
                } else {
                    ++found;
                }
            }

            _starved = false;
        }

        _wake.notify_all();
    }

    void Loader::show(const std::uint64_t generation, const std::filesystem::path &current,
                      std::vector<std::filesystem::path> ahead, std::vector<std::filesystem::path> behind) {
        {
            const std::scoped_lock hold(_guard);

            _request.generation = generation;
            ++_request.sequence;
            _request.current = current;
            _request.ahead = std::move(ahead);
            _request.behind = std::move(behind);
            _request.at = std::chrono::steady_clock::now();
            _starved = false;

            // Moving on cancels a rebuild.
            if (_rebuild.file != current) {
                _rebuild = {};
            }

            if (const auto found = _cache.find(current); found != _cache.end()) {
                found->second.used = _request.sequence;
                post_cached(found->second);
            } else if (const auto job = std::ranges::find_if(_jobs,
                                                             [&](const Job &other) {
                                                                 return other.file == current && other.streaming
                                                                        && !other.abort->requested();
                                                             });
                       job != _jobs.end()) {
                // Shown again while its tiles are being written, say when the mode switched as
                // the job started. It said so for the request before, which the view has dropped.
                post({
                        .generation = generation,
                        .kind = Kind::Building,
                        .info = job->info,
                        .pyramid = nullptr,
                        .error = {},
                        .unsupported = false,
                        .animation = nullptr,
                        .tileCache = nullptr,
                        .tileFolder = job->tileFolder,
                });
            }

            // What the images left behind unpacked of their tiles goes, the tiles themselves stay.
            for (auto &[file, entry] : _cache) {
                if (entry.tileCache != nullptr && file != current) {
                    entry.tileCache->shrink();
                }
            }

            for (const auto *list : {&_request.ahead, &_request.behind}) {
                for (const std::filesystem::path &file : *list) {
                    if (const auto found = _cache.find(file); found != _cache.end()) {
                        found->second.used = _request.sequence;
                    }
                }
            }

            abort_strays();
            trim();
        }

        _wake.notify_all();
    }

    bool Loader::take(Result *out) {
        const std::scoped_lock hold(_guard);

        if (_results.empty()) {
            return false;
        }

        *out = std::move(_results.front());
        _results.pop_front();

        return true;
    }

    bool Loader::cached(const std::filesystem::path &file, Decode::Info *info,
                        std::shared_ptr<const Pyramid> *pyramid) const {
        const std::scoped_lock hold(_guard);

        const auto found = _cache.find(file);

        if (found == _cache.end() || found->second.pyramid == nullptr) {
            return false;
        }

        *info = found->second.info;
        *pyramid = found->second.pyramid;

        return true;
    }

    void Loader::forget(const std::filesystem::path &file) {
        const std::scoped_lock hold(_guard);

        for (const Job &job : _jobs) {
            if (job.file == file) {
                job.abort->request();
            }
        }

        if (const auto found = _cache.find(file); found != _cache.end()) {
            release(found->second);
            _cache.erase(found);
        }
    }

    void Loader::rebuild(const std::filesystem::path &file) {
        const std::scoped_lock hold(_guard);

        _rebuild = {.file = file, .old = {}};

        for (const Job &job : _jobs) {
            if (job.file == file) {
                job.abort->request();
            }
        }

        if (const auto found = _cache.find(file); found != _cache.end()) {
            if (found->second.tileCache != nullptr) {
                _rebuild.old = found->second.tileCache->path();
            }

            release(found->second);
            _cache.erase(found);
        }
    }

    void Loader::reoriented(const std::filesystem::path &file, const int orientation) {
        const std::scoped_lock hold(_guard);

        for (const Job &job : _jobs) {
            if (job.file == file) {
                job.abort->request();
            }
        }

        if (const auto found = _cache.find(file); found != _cache.end()) {
            Decode::Info &info = found->second.info;

            if (Orient::swaps(info.orientation) != Orient::swaps(orientation)) {
                std::swap(info.width, info.height);
            }

            info.orientation = orientation;
        }
    }

    std::size_t Loader::cached_bytes() const {
        const std::scoped_lock hold(_guard);

        return _bytes;
    }

    std::size_t Loader::cached_files() const {
        const std::scoped_lock hold(_guard);

        return _cache.size();
    }

    int Loader::rank(const std::filesystem::path &file) const {
        if (file == _request.current) {
            return 0;
        }

        for (std::size_t i = 0; i < _request.ahead.size(); ++i) {
            if (_request.ahead.at(i) == file) {
                return static_cast<int>(i) + 1;
            }
        }

        for (std::size_t i = 0; i < _request.behind.size(); ++i) {
            if (_request.behind.at(i) == file) {
                return static_cast<int>(_request.ahead.size() + i) + 1;
            }
        }

        return FAR;
    }

    bool Loader::near(const std::filesystem::path &file) const {
        return file == _request.current || (!_request.ahead.empty() && file == _request.ahead.front())
               || (!_request.behind.empty() && file == _request.behind.front());
    }

    bool Loader::running(const std::filesystem::path &file) const {
        return std::ranges::any_of(_jobs, [&](const Job &job) { return job.file == file; });
    }

    bool Loader::rested(const int ms) const {
        return std::chrono::steady_clock::now() - _request.at >= std::chrono::milliseconds(ms);
    }

    // What a file needs decoded, if anything. A file that is not near never needs the whole
    // image, since it would be trimmed to the screen straight away, and only the current one
    // needs its frames.
    bool Loader::wants_job(const std::filesystem::path &file, bool *whole, bool *frames, bool *later) const {
        if (running(file)) {
            return false;
        }

        const auto found = _cache.find(file);

        *whole = false;
        *frames = false;

        if (found == _cache.end()) {
            return true;
        }

        const Entry &entry = found->second;

        // A pyramid on disk costs a pass over the whole file, so only the image on screen makes
        // one, and an HDR one only once it is known what display the pyramid is for.
        if (entry.streamed) {
            return file == _request.current && entry.tileCache == nullptr && entry.error.empty()
                   && (!entry.info.hdr || _displayKnown);
        }

        const bool animate = file == _request.current && entry.info.frames > 1 && entry.animation == nullptr;

        // A scalable image is only ever rendered for the screen, the Refiner draws it sharper.
        const bool done = entry.whole || Decode::scalable(entry.info.kind);

        if (!entry.error.empty() || (done && !animate) || !near(file)) {
            return false;
        }

        if (!rested(REST_MS)) {
            *later = true;

            return false;
        }

        *whole = !entry.whole;
        *frames = entry.whole;

        return true;
    }

    // The nearest file that needs work, the current first, then the neighbours, then ahead by
    // distance, then behind.
    bool Loader::pick(Job *out, bool *later) {
        std::vector<const std::filesystem::path *> order;

        order.push_back(&_request.current);

        if (!_request.ahead.empty()) {
            order.push_back(&_request.ahead.front());
        }

        if (!_request.behind.empty()) {
            order.push_back(&_request.behind.front());
        }

        for (const auto *list : {&_request.ahead, &_request.behind}) {
            for (std::size_t i = 1; i < list->size(); ++i) {
                order.push_back(&list->at(i));
            }
        }

        for (const std::filesystem::path *file : order) {
            if (file->empty()) {
                continue;
            }

            bool whole = false;
            bool frames = false;

            if (!wants_job(*file, &whole, &frames, later)) {
                continue;
            }

            // Nothing beyond the current image starts while memory is short.
            if (_starved && *file != _request.current) {
                continue;
            }

            out->file = *file;
            out->whole = whole;
            out->frames = frames;
            out->abort = std::make_shared<Decode::Abort>();
            out->estimate = 0;

            _jobs.push_back(*out);

            return true;
        }

        return false;
    }

    void Loader::work() {
        std::unique_lock hold(_guard);

        while (_running) {
            if (!_trash.empty()) {
                std::vector<std::shared_ptr<const void>> trash;

                trash.swap(_trash);
                hold.unlock();
                trash.clear();
                hold.lock();
                _freed = true;

                continue;
            }

            Job job;
            bool later = false;

            if (!pick(&job, &later)) {
                if (_freed && _jobs.empty() && !later) {
                    if (!rested(GIVE_BACK_MS)) {
                        _wake.wait_until(hold, _request.at + std::chrono::milliseconds(GIVE_BACK_MS));

                        continue;
                    }

                    _freed = false;
                    hold.unlock();
                    Memory::give_back();
                    hold.lock();

                    continue;
                }

                if (later) {
                    _wake.wait_until(hold, _request.at + std::chrono::milliseconds(REST_MS));
                } else {
                    _wake.wait(hold);
                }

                continue;
            }

            hold.unlock();
            decode(job);
            hold.lock();
            _freed = true;
        }
    }

    bool Loader::idle() const {
        const std::scoped_lock hold(_guard);

        return _jobs.empty();
    }

    void Loader::stream_all(const bool on) {
        {
            const std::scoped_lock hold(_guard);

            if (_streamAll == on) {
                return;
            }

            _streamAll = on;

            // Images too large for memory stream in either mode, so their tiles and builds stay.
            for (const Job &job : _jobs) {
                if (!job.streaming || job.info.pixels() <= max_pixels()) {
                    job.abort->request();
                }
            }

            for (auto at = _cache.begin(); at != _cache.end();) {
                if (at->second.streamed && at->second.info.pixels() > max_pixels()) {
                    ++at;

                    continue;
                }

                release(at->second);
                at = _cache.erase(at);
            }

            _starved = false;
        }

        _wake.notify_all();
    }

    bool Loader::streaming_all() const {
        const std::scoped_lock hold(_guard);

        return _streamAll;
    }

    // Accounts for a decode about to start. False when the budget cannot take it yet, in
    // which case nothing but the current image starts until memory frees up.
    bool Loader::admit(const Job &job, const std::size_t estimate) {
        const std::scoped_lock hold(_guard);

        const auto found = std::ranges::find_if(_jobs, [&](const Job &other) { return other.file == job.file; });

        if (found == _jobs.end()) {
            return false;
        }

        if (job.file != _request.current && _bytes + _inflight + estimate > _budget) {
            trim();

            if (_bytes + _inflight + estimate > _budget) {
                _starved = true;
                _jobs.erase(found);

                return false;
            }
        }

        found->estimate = estimate;
        _inflight += estimate;

        return true;
    }

    // Forgets the job for the file and the memory it was expected to take. Under the lock.
    // True when the job had been told to stop.
    bool Loader::finish(const std::filesystem::path &file) {
        const auto job = std::ranges::find_if(_jobs, [&](const Job &other) { return other.file == file; });

        if (job == _jobs.end()) {
            return false;
        }

        const bool stopped = job->abort->requested();

        _inflight -= std::min(_inflight, job->estimate);
        _jobs.erase(job);

        return stopped;
    }

    void Loader::decode(Job &job) {
        if (job.frames) {
            decode_frames(job);

            return;
        }

        Entry entry;
        Decode::Abort &abort = *job.abort;

        const auto drop = [&] {
            const std::scoped_lock hold(_guard);

            finish(job.file);
            _wake.notify_all();
        };

        if (!Decode::probe(job.file, &entry.info, &entry.error)) {
            entry.unsupported = !Decode::recognised(job.file);
            store(job.file, std::move(entry));

            return;
        }

        bool all = false;

        {
            const std::scoped_lock hold(_guard);

            all = _streamAll;
        }

        // Too large for memory, or in streaming mode, a still image shows from disk. Drawings
        // and animations have no pyramid to stream, so they stay in memory.
        const bool still = !Decode::scalable(entry.info.kind) && entry.info.frames <= 1;

        if (still && (all || entry.info.pixels() > max_pixels())) {
            decode_stream(job, std::move(entry));

            return;
        }

        int screenWidth = 0;
        int screenHeight = 0;

        {
            const std::scoped_lock hold(_guard);

            screenWidth = _screenWidth;
            screenHeight = _screenHeight;
            entry.display = display_for(entry.info);
        }

        const bool scalable = Decode::scalable(entry.info.kind);
        bool over = entry.info.pixels() > max_pixels();
        const Plan planned = plan(entry.info, job.whole, over, _budget, screenWidth, screenHeight);
        bool whole = planned.whole;

        if (!admit(job, planned.estimate)) {
            return;
        }

        bool starved = false;

        const auto produce = [&](const Box at, const Decode::Fit how, const std::int64_t limit) {
            try {
                Bitmap decoded;

                if (!Decode::load(job.file, at.width, at.height, &decoded, &entry.error, &abort, how, entry.display)) {
                    return false;
                }

                while (decoded.pixels() > limit) {
                    decoded = Pyramid::halve(decoded);
                }

                // A cheap preview of an image the screen holds is the whole image.
                if (!whole && decoded.pixels() >= entry.info.pixels()) {
                    whole = true;
                }

                if (!over && whole && !scalable && entry.info.kind != Decode::Format::Other) {
                    take_size(decoded, &entry.info);
                }

                entry.pyramid = std::make_shared<const Pyramid>(
                        Pyramid::build(std::move(decoded), screenWidth / THUMB_DIVISOR, screenHeight / THUMB_DIVISOR));

                return true;
            } catch (const std::bad_alloc &) {
                starved = true;
                entry.error = "not enough memory";

                return false;
            }
        };

        bool loaded = produce(planned.box, planned.fit, max_pixels());

        // The system had too little left for the whole image, so it streams down to the size
        // every machine is expected to hold.
        if (!loaded && starved && whole && !scalable && !abort.requested()) {
            over = true;
            entry.error.clear();
            loaded = produce(capped(entry.info, MIN_PIXELS), Decode::Fit::Force, MIN_PIXELS);
        }

        if (abort.requested()) {
            drop();

            return;
        }

        if (loaded) {
            entry.whole = whole;
        }

        store(job.file, std::move(entry));
    }

    // Opens the pyramid made for the file before, in either cache mode, or makes one when it is the
    // current image. The others wait for a pyramid until they are shown, since it takes a pass over
    // the whole file. An image that fits in memory, streamed only because streaming mode is on, keeps
    // its pyramid in memory and never touches the disk, unless every stream persists or a rebuild
    // replaces one on disk.
    void Loader::decode_stream(Job &job, Entry entry) {
        entry.streamed = true;

        bool waiting = false;
        bool small = false;
        bool persist = false;
        bool rebuild = false;
        std::filesystem::path old;
        std::filesystem::path folder;

        {
            const std::scoped_lock hold(_guard);

            entry.display = display_for(entry.info);
            waiting = entry.info.hdr && !_displayKnown;
            folder = _tileFolder;
            small = _smallTiles;
            persist = _persistTiles;
            rebuild = job.file == _rebuild.file && !job.abort->requested();
            old = _rebuild.old;
        }

        // Kept without a tile cache, and wants_job() picks it up again once the display is said.
        if (waiting) {
            store(job.file, std::move(entry));

            return;
        }

        const TileCache::Store where = (rebuild && !old.empty()) || persist || entry.info.pixels() > max_pixels()
                                               ? TileCache::Store::Disk
                                               : TileCache::Store::Memory;

        if (!rebuild) {
            entry.tileCache = TileCache::open(job.file, folder, entry.display);
        }

        bool build = false;
        bool current = false;

        {
            const std::scoped_lock hold(_guard);

            current = job.file == _request.current;
        }

        // Said while the tiles are made, so where the room goes is never hidden. Only the image that
        // builds makes its folder.
        const std::filesystem::path shownFolder =
                where == TileCache::Store::Disk && entry.tileCache == nullptr && current
                        ? TileCache::location(job.file, folder)
                        : std::filesystem::path{};

        {
            const std::scoped_lock hold(_guard);

            if (const auto found =
                        std::ranges::find_if(_jobs, [&](const Job &other) { return other.file == job.file; });
                found != _jobs.end()) {
                found->streaming = true;
                found->info = entry.info;
                found->tileFolder = shownFolder;
            }

            build = entry.tileCache == nullptr && job.file == _request.current;

            if (build && rebuild) {
                _rebuild = {};
            }

            if (build) {
                _progress.store(0.0F, std::memory_order_relaxed);
                post({
                        .generation = _request.generation,
                        .kind = Kind::Building,
                        .info = entry.info,
                        .pyramid = nullptr,
                        .error = {},
                        .unsupported = false,
                        .animation = nullptr,
                        .tileCache = nullptr,
                        .tileFolder = shownFolder,
                });
            }
        }

        // Else it outlives a new one made under another name or folder.
        if (build && rebuild && !old.empty()) {
            std::error_code failure;

            std::filesystem::remove(old, failure);
        }

        if (build) {
            // A decoder that holds much of the image would take the system down with it.
            const std::uint64_t needed = Decode::stream_bytes(job.file, TileCache::TILE);
            const std::uint64_t free = Memory::available();

            if (needed > free) {
                entry.error =
                        std::format("{}: needs {:.1f} GB of memory to decode, {:.1f} GB is free", job.file.string(),
                                    static_cast<double>(needed) / 1e9, static_cast<double>(free) / 1e9);
            } else {
                entry.tileCache = TileCache::build(job.file, folder, entry.display, where, small, &_progress,
                                                   &entry.error, job.abort.get());
            }
        }

        if (job.abort->requested()) {
            const std::scoped_lock hold(_guard);

            finish(job.file);
            _wake.notify_all();

            return;
        }

        store(job.file, std::move(entry));
    }

    void Loader::decode_frames(Job &job) {
        const auto drop = [&] {
            const std::scoped_lock hold(_guard);

            finish(job.file);
            _wake.notify_all();
        };

        Decode::Info info;
        int thumbWidth = 0;
        int thumbHeight = 0;

        {
            const std::scoped_lock hold(_guard);

            if (const auto found = _cache.find(job.file); found != _cache.end()) {
                info = found->second.info;
            }

            thumbWidth = _screenWidth / THUMB_DIVISOR;
            thumbHeight = _screenHeight / THUMB_DIVISOR;
        }

        if (info.frames < 2) {
            drop();

            return;
        }

        const std::size_t estimate =
                std::min(pyramid_bytes(capped(info)) * static_cast<std::size_t>(info.frames), MAX_ANIMATION_BYTES);

        if (!admit(job, estimate)) {
            return;
        }

        std::vector<Decode::Frame> frames;

        // The levels below each frame add up to a third of it again.
        if (!Decode::load_frames(job.file, MAX_ANIMATION_BYTES / 4 * 3, &frames, nullptr, job.abort.get())) {
            if (job.abort->requested()) {
                drop();
            } else {
                store_frames(job.file, nullptr);
            }

            return;
        }

        auto animation = std::make_shared<Animation>();

        animation->frames.reserve(frames.size());

        for (Decode::Frame &frame : frames) {
            if (job.abort->requested()) {
                drop();

                return;
            }

            animation->frames.push_back({
                    .pyramid = std::make_shared<const Pyramid>(
                            Pyramid::build(std::move(frame.bitmap), thumbWidth, thumbHeight)),
                    .delay = frame.delay,
            });
        }

        store_frames(job.file, std::move(animation));
    }

    void Loader::store(const std::filesystem::path &file, Entry entry) {
        {
            const std::scoped_lock hold(_guard);

            // Stopped too late to cut short, and it may have read what the file said before. An
            // HDR image decoded for a display since replaced is dropped the same way.
            const bool stale = entry.info.hdr && entry.display.headroom != _display.headroom;

            if (finish(file) || stale) {
                if (entry.pyramid != nullptr) {
                    _trash.push_back(std::move(entry.pyramid));
                }

                if (entry.tileCache != nullptr) {
                    _trash.push_back(std::move(entry.tileCache));
                }
            } else {
                insert(file, std::move(entry));
            }
        }

        _wake.notify_all();
    }

    // Under the lock.
    void Loader::insert(const std::filesystem::path &file, Entry entry) {
        if (const auto old = _cache.find(file); old != _cache.end()) {
            release(old->second);
            _cache.erase(old);
        }

        entry.used = _request.sequence;

        if (entry.pyramid != nullptr) {
            _bytes += entry.pyramid->bytes();
        }

        if (entry.tileCache != nullptr) {
            _bytes += entry.tileCache->stored_bytes();
        }

        const Entry &kept = _cache.emplace(file, std::move(entry)).first->second;

        if (file == _request.current) {
            post_cached(kept);
        }

        _starved = false;
        trim();
    }

    void Loader::store_frames(const std::filesystem::path &file, std::shared_ptr<const Animation> animation) {
        {
            const std::scoped_lock hold(_guard);

            finish(file);

            if (const auto found = _cache.find(file); found != _cache.end()) {
                Entry &entry = found->second;

                release_animation(entry);

                if (animation == nullptr) {
                    entry.info.frames = 1;
                } else {
                    _bytes += animation->bytes();
                    entry.animation = std::move(animation);
                }

                if (file == _request.current) {
                    post_cached(entry);
                }

                trim();
            } else if (animation != nullptr) {
                _trash.push_back(std::move(animation));
            }
        }

        _wake.notify_all();
    }

    void Loader::release(Entry &entry) {
        if (entry.pyramid != nullptr) {
            _bytes -= entry.pyramid->bytes();
            _trash.push_back(std::move(entry.pyramid));
        }

        if (entry.tileCache != nullptr) {
            _bytes -= entry.tileCache->stored_bytes();
            _trash.push_back(std::move(entry.tileCache));
        }

        release_animation(entry);
    }

    void Loader::release_animation(Entry &entry) {
        if (entry.animation != nullptr) {
            _bytes -= entry.animation->bytes();
            _trash.push_back(std::move(entry.animation));
        }
    }

    // Decodes for files navigation has left behind stop, and so do whole decodes for files
    // that are no longer neighbours, since they would be trimmed on arrival.
    void Loader::abort_strays() {
        for (const Job &job : _jobs) {
            const int at = rank(job.file);

            if (at == FAR || (job.whole && !near(job.file))
                || ((job.frames || job.streaming) && job.file != _request.current)) {
                job.abort->request();
            }
        }
    }

    // Full resolution and frames stay only for the current image and its neighbours. Over budget, the
    // farthest files go, least recently wanted first, then the far end of the window, then the neighbours'
    // frames, and last the neighbours drop to the screen. The current image is never touched.
    void Loader::trim() {
        evict_outside();

        for (auto &[file, entry] : _cache) {
            if (entry.whole && entry.pyramid != nullptr && !near(file)) {
                cut(entry);
            }

            if (!near(file)) {
                release_animation(entry);
            }
        }

        while (_bytes > _budget && evict_one()) {
        }

        for (auto &[file, entry] : _cache) {
            if (_bytes > _budget && file != _request.current) {
                release_animation(entry);
            }
        }

        for (auto &[file, entry] : _cache) {
            if (_bytes > _budget && entry.whole && entry.pyramid != nullptr && file != _request.current) {
                cut(entry);
            }
        }
    }

    void Loader::cut(Entry &entry) {
        const Pyramid trimmed = entry.pyramid->trimmed(_screenWidth, _screenHeight);

        if (trimmed.levels.size() != entry.pyramid->levels.size()) {
            _bytes -= entry.pyramid->bytes();
            _trash.push_back(std::move(entry.pyramid));
            entry.pyramid = std::make_shared<const Pyramid>(trimmed);
            _bytes += entry.pyramid->bytes();
        }

        entry.whole = false;
    }

    // Outside the window, only as many files stay as turning back would find ahead and not
    // behind, the most recently wanted. The rest would sit there until the budget ran out.
    void Loader::evict_outside() {
        const std::size_t spare = _request.ahead.size() - std::min(_request.ahead.size(), _request.behind.size());
        std::vector<std::map<std::filesystem::path, Entry>::iterator> outside;

        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            if (rank(it->first) == FAR) {
                outside.push_back(it);
            }
        }

        if (outside.size() <= spare) {
            return;
        }

        std::ranges::sort(outside, std::ranges::greater{}, [](const auto &it) { return it->second.used; });

        for (const auto &it : std::span(outside).subspan(spare)) {
            release(it->second);
            _cache.erase(it);
        }
    }

    // Drops the entry farthest from the current image, the least recently wanted first at
    // equal distance, never the current image or a neighbour. False when there is none.
    bool Loader::evict_one() {
        auto victim = _cache.end();
        int worst = 1;

        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            const int at = rank(it->first);

            const bool holds = it->second.pyramid != nullptr
                               || (it->second.tileCache != nullptr && it->second.tileCache->stored_bytes() > 0);

            if (at <= 1 || !holds) {
                continue;
            }

            if (victim == _cache.end() || at > worst || (at == worst && it->second.used < victim->second.used)) {
                victim = it;
                worst = at;
            }
        }

        if (victim == _cache.end()) {
            return false;
        }

        release(victim->second);
        _cache.erase(victim);

        return true;
    }

    void Loader::post_cached(const Entry &entry) {
        if (!entry.error.empty()) {
            post({
                    .generation = _request.generation,
                    .kind = Kind::Failed,
                    .info = entry.info,
                    .pyramid = nullptr,
                    .error = entry.error,
                    .unsupported = entry.unsupported,
                    .animation = nullptr,
                    .tileCache = nullptr,
                    .tileFolder = {},
            });

            return;
        }

        if (entry.streamed) {
            if (entry.tileCache != nullptr) {
                post({
                        .generation = _request.generation,
                        .kind = Kind::Full,
                        .info = entry.info,
                        .pyramid = nullptr,
                        .error = {},
                        .unsupported = false,
                        .animation = nullptr,
                        .tileCache = entry.tileCache,
                        .tileFolder = {},
                });
            } else {
                _progress.store(0.0F, std::memory_order_relaxed);
                post({
                        .generation = _request.generation,
                        .kind = Kind::Building,
                        .info = entry.info,
                        .pyramid = nullptr,
                        .error = {},
                        .unsupported = false,
                        .animation = nullptr,
                        .tileCache = nullptr,
                        .tileFolder = {},
                });
            }

            return;
        }

        const bool full = entry.whole || Decode::scalable(entry.info.kind);

        post({
                .generation = _request.generation,
                .kind = full ? Kind::Full : Kind::Preview,
                .info = entry.info,
                .pyramid = entry.pyramid,
                .error = {},
                .unsupported = false,
                .animation = entry.animation,
                .tileCache = nullptr,
                .tileFolder = {},
        });
    }

    void Loader::post(Result result) {
        _results.push_back(std::move(result));

        SDL_Event event{};

        event.type = _event;

        SDL_PushEvent(&event);
    }
}
