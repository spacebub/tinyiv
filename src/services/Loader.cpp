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
#include "image/Decode.h"
#include "image/Orient.h"
#include "image/Pyramid.h"
#include "image/Store.h"
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
                return {info.width, info.height};
            }

            const double shrink = std::sqrt(static_cast<double>(limit) / static_cast<double>(info.pixels()));

            return {std::max(static_cast<int>(std::floor(info.width * shrink)), 1), std::max(static_cast<int>(std::floor(info.height * shrink)), 1)};
        }

        Box fitted(const Decode::Info &info, const int boxWidth, const int boxHeight) {
            if (info.width <= boxWidth && info.height <= boxHeight) {
                return {info.width, info.height};
            }

            const double shrink = std::min(static_cast<double>(boxWidth) / info.width, static_cast<double>(boxHeight) / info.height);

            return {std::max(static_cast<int>(std::floor(info.width * shrink)), 1), std::max(static_cast<int>(std::floor(info.height * shrink)), 1)};
        }

        // Fits the box, enlarged if need be.
        Box filled(const Decode::Info &info, const int boxWidth, const int boxHeight) {
            const double scale = std::min(static_cast<double>(boxWidth) / info.width, static_cast<double>(boxHeight) / info.height);

            return {std::max(static_cast<int>(std::floor(info.width * scale)), 1), std::max(static_cast<int>(std::floor(info.height * scale)), 1)};
        }

        void take_size(const Bitmap &decoded, Decode::Info *info) {
            const bool swapped = Orient::swaps(info->orientation);

            info->width = swapped ? decoded.height() : decoded.width();
            info->height = swapped ? decoded.width() : decoded.height();
        }

        // A pyramid is its base and a third again.
        std::size_t pyramid_bytes(const Box box) {
            return static_cast<std::size_t>(box.width) * static_cast<std::size_t>(box.height) * Bitmap::CHANNELS * 4 / 3;
        }
    }

    std::int64_t Loader::max_pixels() {
        static const std::int64_t held = [] {
            // SDL counts whole mebibytes.
            const std::int64_t half = static_cast<std::int64_t>(SDL_GetSystemRAM()) * 1024 * 1024 / 2;

            // A pyramid is four bytes a pixel and a third again.
            return std::max(half * 3 / (Bitmap::CHANNELS * 4), MIN_PIXELS);
        }();

        return held;
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

            // What the images left behind read of their files goes, what is on disk stays.
            for (auto &[file, entry] : _cache) {
                if (entry.store != nullptr && file != current) {
                    entry.store->shrink();
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

        // A pyramid on disk costs a pass over the whole file, so only the image on screen makes one.
        if (entry.streamed) {
            return file == _request.current && entry.store == nullptr && entry.error.empty();
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

            for (const Job &job : _jobs) {
                job.abort->request();
            }

            for (auto &[file, entry] : _cache) {
                release(entry);
            }

            _cache.clear();
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
        }

        const bool scalable = Decode::scalable(entry.info.kind);
        const bool cheap = Decode::scales_cheaply(entry.info.kind);
        bool over = entry.info.pixels() > max_pixels();
        const std::size_t nativeBytes = static_cast<std::size_t>(entry.info.pixels()) * Bitmap::CHANNELS;
        // WebP and JXL have no cheaper decode than the native size, which is halved down after,
        // as long as the transient bitmap fits the budget.
        const bool nativeThenHalve = over && (entry.info.kind == Decode::Format::WebP || entry.info.kind == Decode::Format::Jxl) && nativeBytes <= _budget;

        Box box{};
        Decode::Fit fit = Decode::Fit::Cheap;
        bool whole = true;

        if (scalable) {
            box = filled(entry.info, screenWidth, screenHeight);
        } else if (!job.whole && cheap) {
            box = fitted(entry.info, screenWidth, screenHeight);
            whole = false;
        } else if (over && !nativeThenHalve) {
            box = capped(entry.info);
            fit = Decode::Fit::Force;
        } else {
            box = capped(entry.info);
        }

        std::size_t estimate = pyramid_bytes(whole && !scalable ? capped(entry.info) : box);

        if (whole && nativeThenHalve) {
            estimate += nativeBytes;
        }

        if (!admit(job, estimate)) {
            return;
        }

        bool starved = false;

        const auto produce = [&](const Box at, const Decode::Fit how, const std::int64_t limit) {
            try {
                Bitmap decoded;

                if (!Decode::load(job.file, at.width, at.height, &decoded, &entry.error, &abort, how)) {
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

                entry.pyramid = std::make_shared<const Pyramid>(Pyramid::build(std::move(decoded), screenWidth / THUMB_DIVISOR, screenHeight / THUMB_DIVISOR));

                return true;
            } catch (const std::bad_alloc &) {
                starved = true;
                entry.error = "not enough memory";

                return false;
            }
        };

        bool loaded = produce(box, fit, max_pixels());

        // The system had too little left for the whole image, so it streams down to the size
        // every machine is expected to hold.
        if (!loaded && starved && whole && !scalable && !abort.requested()) {
            over = true;
            entry.error.clear();
            loaded = produce(capped(entry.info, MIN_PIXELS), Decode::Fit::Force, MIN_PIXELS);
        }

        if (!loaded) {
            if (abort.requested()) {
                drop();
            } else {
                store(job.file, std::move(entry));
            }

            return;
        }

        entry.whole = whole;

        if (abort.requested()) {
            drop();

            return;
        }

        store(job.file, std::move(entry));
    }

    // Opens the pyramid made for the file before, or makes one when it is the current image.
    // The others wait for a pyramid until they are shown, since it takes a pass over the whole file.
    void Loader::decode_stream(Job &job, Entry entry) {
        entry.streamed = true;
        entry.store = Store::open(job.file);

        bool build = false;

        {
            const std::scoped_lock hold(_guard);

            if (const auto found = std::ranges::find_if(_jobs, [&](const Job &other) { return other.file == job.file; }); found != _jobs.end()) {
                found->streaming = true;
            }

            build = entry.store == nullptr && job.file == _request.current;

            if (build) {
                _progress.store(0.0F, std::memory_order_relaxed);
                post({_request.generation, Kind::Building, entry.info, nullptr, {}, false, nullptr, nullptr});
            }
        }

        if (build) {
            entry.store = Store::build(job.file, &_progress, &entry.error, job.abort.get());
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

        const std::size_t estimate = std::min(pyramid_bytes(capped(info)) * static_cast<std::size_t>(info.frames), MAX_ANIMATION_BYTES);

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

            animation->frames.push_back({std::make_shared<const Pyramid>(Pyramid::build(std::move(frame.bitmap), thumbWidth, thumbHeight)), frame.delay});
        }

        store_frames(job.file, std::move(animation));
    }

    void Loader::store(const std::filesystem::path &file, Entry entry) {
        {
            const std::scoped_lock hold(_guard);

            // Stopped too late to cut short, and it may have read what the file said before.
            if (finish(file)) {
                if (entry.pyramid != nullptr) {
                    _trash.push_back(std::move(entry.pyramid));
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

        if (entry.store != nullptr) {
            _trash.push_back(std::move(entry.store));
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

            if (at == FAR || (job.whole && !near(job.file)) || ((job.frames || job.streaming) && job.file != _request.current)) {
                job.abort->request();
            }
        }
    }

    // Full resolution and frames stay only for the current image and its neighbours. Beyond
    // the budget, the files farthest from the current go, least recently wanted first, then
    // the far end of the window itself, then the neighbours' frames, and last of all the
    // neighbours drop to the screen. The current image is never touched.
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

        release(victim->second);
        _cache.erase(victim);

        return true;
    }

    void Loader::post_cached(const Entry &entry) {
        if (!entry.error.empty()) {
            post({_request.generation, Kind::Failed, entry.info, nullptr, entry.error, entry.unsupported, nullptr});

            return;
        }

        if (entry.streamed) {
            if (entry.store != nullptr) {
                post({_request.generation, Kind::Full, entry.info, nullptr, {}, false, nullptr, entry.store});
            } else {
                _progress.store(0.0F, std::memory_order_relaxed);
                post({_request.generation, Kind::Building, entry.info, nullptr, {}, false, nullptr, nullptr});
            }

            return;
        }

        const bool full = entry.whole || Decode::scalable(entry.info.kind);

        post({_request.generation, full ? Kind::Full : Kind::Preview, entry.info, entry.pyramid, {}, false, entry.animation});
    }

    void Loader::post(Result result) {
        _results.push_back(std::move(result));

        SDL_Event event{};

        event.type = _event;

        SDL_PushEvent(&event);
    }
}
