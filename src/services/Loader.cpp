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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "image/Decode.h"
#include "image/Pyramid.h"
#include "services/Loader.h"

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

        Box capped(const Decode::Info &info) {
            if (info.pixels() <= Loader::MAX_PIXELS) {
                return {info.width, info.height};
            }

            const double shrink = std::sqrt(static_cast<double>(Loader::MAX_PIXELS) / static_cast<double>(info.pixels()));

            return {std::max(static_cast<int>(std::floor(info.width * shrink)), 1), std::max(static_cast<int>(std::floor(info.height * shrink)), 1)};
        }

        Box fitted(const Decode::Info &info, const int boxWidth, const int boxHeight) {
            if (info.width <= boxWidth && info.height <= boxHeight) {
                return {info.width, info.height};
            }

            const double shrink = std::min(static_cast<double>(boxWidth) / info.width, static_cast<double>(boxHeight) / info.height);

            return {std::max(static_cast<int>(std::floor(info.width * shrink)), 1), std::max(static_cast<int>(std::floor(info.height * shrink)), 1)};
        }

        // A pyramid is its base and a third again.
        std::size_t pyramid_bytes(const Box box) {
            return static_cast<std::size_t>(box.width) * static_cast<std::size_t>(box.height) * Bitmap::CHANNELS * 4 / 3;
        }
    }

    Loader::Loader(const std::uint32_t eventType, const std::size_t cacheBytes, const int workers) : _event(eventType), _budget(cacheBytes) {
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

    void Loader::show(const std::uint64_t generation, const std::filesystem::path &current, std::vector<std::filesystem::path> ahead, std::vector<std::filesystem::path> behind) {
        {
            const std::scoped_lock hold(_guard);

            _request.generation = generation;
            ++_request.sequence;
            _request.current = current;
            _request.ahead = std::move(ahead);
            _request.behind = std::move(behind);
            _request.at = std::chrono::steady_clock::now();
            _starved = false;

            if (const auto found = _cache.find(current); found != _cache.end()) {
                found->second.used = _request.sequence;
                post_cached(found->second);
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

    bool Loader::cached(const std::filesystem::path &file, Decode::Info *info, std::shared_ptr<const Pyramid> *pyramid) const {
        const std::scoped_lock hold(_guard);

        const auto found = _cache.find(file);

        if (found == _cache.end() || found->second.pyramid == nullptr) {
            return false;
        }

        *info = found->second.info;
        *pyramid = found->second.pyramid;

        return true;
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
    // image, since it would be trimmed to the screen straight away.
    bool Loader::wants_job(const std::filesystem::path &file, bool *whole, bool *later) const {
        if (running(file)) {
            return false;
        }

        const auto found = _cache.find(file);

        if (found == _cache.end()) {
            *whole = false;

            return true;
        }

        if (!found->second.error.empty() || found->second.whole || !near(file)) {
            return false;
        }

        if (!rested(REST_MS)) {
            *later = true;

            return false;
        }

        *whole = true;

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

            if (!wants_job(*file, &whole, later)) {
                continue;
            }

            // Nothing beyond the current image starts while memory is short.
            if (_starved && *file != _request.current) {
                continue;
            }

            out->file = *file;
            out->whole = whole;
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
                std::vector<std::shared_ptr<const Pyramid>> trash;

                trash.swap(_trash);
                hold.unlock();
                trash.clear();
                hold.lock();

                continue;
            }

            Job job;
            bool later = false;

            if (!pick(&job, &later)) {
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
        }
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

    void Loader::decode(Job &job) {
        Entry entry;
        Decode::Abort &abort = *job.abort;

        const auto drop = [&] {
            const std::scoped_lock hold(_guard);

            std::erase_if(_jobs, [&](const Job &other) { return other.file == job.file; });
            _wake.notify_all();
        };

        if (!Decode::probe(job.file, &entry.info, &entry.error)) {
            store(job.file, std::move(entry));

            return;
        }

        int screenWidth = 0;
        int screenHeight = 0;

        {
            const std::scoped_lock hold(_guard);

            screenWidth = _screenWidth;
            screenHeight = _screenHeight;
        }

        const bool cheap = Decode::scales_cheaply(entry.info.kind);
        const bool over = entry.info.pixels() > MAX_PIXELS;
        const std::size_t nativeBytes = static_cast<std::size_t>(entry.info.pixels()) * Bitmap::CHANNELS;
        // WebP and JXL have no cheaper decode than the native size, which is halved down after,
        // as long as the transient bitmap fits the budget.
        const bool nativeThenHalve = over && !cheap && Decode::direct(entry.info.kind) && entry.info.kind != Decode::Format::Png && nativeBytes <= _budget;

        Box box{};
        Decode::Fit fit = Decode::Fit::Cheap;
        bool whole = true;

        if (!job.whole && cheap) {
            box = fitted(entry.info, screenWidth, screenHeight);
            whole = false;
        } else if (over && !nativeThenHalve) {
            box = capped(entry.info);
            fit = Decode::Fit::Force;
        } else {
            box = capped(entry.info);
        }

        std::size_t estimate = pyramid_bytes(whole ? capped(entry.info) : box);

        if (whole && nativeThenHalve) {
            estimate += nativeBytes;
        }

        if (!admit(job, estimate)) {
            return;
        }

        Bitmap decoded;

        if (!Decode::load(job.file, box.width, box.height, &decoded, &entry.error, &abort, fit)) {
            if (abort.requested()) {
                drop();
            } else {
                store(job.file, std::move(entry));
            }

            return;
        }

        while (decoded.pixels() > MAX_PIXELS) {
            decoded = Pyramid::halve(decoded);
        }

        // A cheap preview of an image the screen holds is the whole image.
        if (!whole && decoded.pixels() >= entry.info.pixels()) {
            whole = true;
        }

        if (!over && whole && entry.info.kind != Decode::Format::Other) {
            entry.info.width = decoded.width();
            entry.info.height = decoded.height();
        }

        entry.pyramid = std::make_shared<const Pyramid>(Pyramid::build(std::move(decoded), screenWidth / THUMB_DIVISOR, screenHeight / THUMB_DIVISOR));
        entry.whole = whole;

        if (abort.requested()) {
            drop();

            return;
        }

        store(job.file, std::move(entry));
    }

    void Loader::store(const std::filesystem::path &file, Entry entry) {
        {
            const std::scoped_lock hold(_guard);

            if (const auto job = std::ranges::find_if(_jobs, [&](const Job &other) { return other.file == file; }); job != _jobs.end()) {
                _inflight -= std::min(_inflight, job->estimate);
                _jobs.erase(job);
            }

            if (const auto old = _cache.find(file); old != _cache.end()) {
                if (old->second.pyramid != nullptr) {
                    _bytes -= old->second.pyramid->bytes();
                    _trash.push_back(std::move(old->second.pyramid));
                }

                _cache.erase(old);
            }

            entry.used = _request.sequence;

            if (entry.pyramid != nullptr) {
                _bytes += entry.pyramid->bytes();
            }

            const Entry &kept = _cache.emplace(file, std::move(entry)).first->second;

            if (file == _request.current) {
                post_cached(kept);
            }

            _starved = false;
            trim();
        }

        _wake.notify_all();
    }

    // Decodes for files navigation has left behind stop, and so do whole decodes for files
    // that are no longer neighbours, since they would be trimmed on arrival.
    void Loader::abort_strays() {
        for (const Job &job : _jobs) {
            const int at = rank(job.file);

            if (at == FAR || (job.whole && !near(job.file))) {
                job.abort->request();
            }
        }
    }

    // Full resolution stays only for the current image and its neighbours. Beyond the
    // budget, the files farthest from the current go, least recently wanted first, then the
    // far end of the window itself, and last of all the neighbours drop to the screen. The
    // current image is never touched.
    void Loader::trim() {
        for (auto &[file, entry] : _cache) {
            if (entry.whole && entry.pyramid != nullptr && !near(file)) {
                cut(entry);
            }
        }

        while (_bytes > _budget && evict_one()) {
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

    // Drops the entry farthest from the current image, the least recently wanted first at
    // equal distance, never the current image or a neighbour. False when there is none.
    bool Loader::evict_one() {
        auto victim = _cache.end();
        int worst = 1;

        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            const int at = rank(it->first);

            if (at <= 1 || it->second.pyramid == nullptr) {
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

        _bytes -= victim->second.pyramid->bytes();
        _trash.push_back(std::move(victim->second.pyramid));
        _cache.erase(victim);

        return true;
    }

    void Loader::post_cached(const Entry &entry) {
        if (!entry.error.empty()) {
            post({_request.generation, Kind::Failed, entry.info, nullptr, entry.error});

            return;
        }

        post({_request.generation, entry.whole ? Kind::Full : Kind::Preview, entry.info, entry.pyramid, {}});
    }

    void Loader::post(Result result) {
        _results.push_back(std::move(result));

        SDL_Event event{};

        event.type = _event;

        SDL_PushEvent(&event);
    }
}
