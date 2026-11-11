// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/App.h"
#include "app/Hud.h"
#include "app/Picture.h"
#include "image/Orient.h"
#include "image/Reorient.h"
#include "image/decode/Decode.h"
#include "services/Loader.h"
#include "services/Memory.h"
#include "services/Refiner.h"
#include "view/Viewport.h"

namespace tiv {
namespace {
constexpr int PREFETCH_AHEAD = 6;
constexpr int PREFETCH_BEHIND = 2;
// A scalable image renders this share of the view beyond each edge, so a short pan stays sharp.
constexpr double REFINE_MARGIN = 0.25;
}

void Picture::attach(const Parts &parts) {
    _folder = parts.folder;
    _viewport = parts.viewport;
    _playback = parts.playback;
    _window = parts.window;
    _loader = parts.loader;
    _refiner = parts.refiner;
    _dirty = parts.dirty;
    _seeking = parts.seeking;
    _tileCacheEvent = parts.tileCacheEvent;
    _footprint = parts.footprint;
}

void Picture::show(const int index, const int direction) {
    if (_folder->count() == 0) {
        return;
    }

    _folder->step(_folder->wrap(index) - _folder->index());
    _direction = direction;

    std::error_code failure;

    _bytes = std::filesystem::file_size(_folder->current(), failure);
    _info = {};
    _turn = 1;
    _message.clear();
    _loading = true;
    _building = false;
    _streamed = false;
    _stale = false;
    _failure.clear();
    *_seeking = false;
    _playback->stop();
    _refiner->cancel();
    _asked = {};

    std::vector<std::filesystem::path> ahead;
    std::vector<std::filesystem::path> behind;
    const int count = _footprint ? 0 : _folder->count();

    for (int i = 1; i <= PREFETCH_AHEAD && i < count; ++i) {
        ahead.push_back(_folder->at(_folder->index() + (i * direction)));
    }

    for (int i = 1; i <= PREFETCH_BEHIND && i + PREFETCH_AHEAD < count; ++i) {
        behind.push_back(_folder->at(_folder->index() - (i * direction)));
    }

    _loader->show(++_generation, _folder->current(), std::move(ahead), std::move(behind));
    _shownAt = SDL_GetTicks();
    *_dirty = true;
}

void Picture::deliver() {
    Loader::Result result;

    while (_loader->take(&result)) {
        if (result.generation != _generation) {
            continue;
        }

        if (result.kind == Loader::Kind::Failed) {
            std::println(stderr, "tinyiv: {}", result.error);

            // The previous image stays on the canvas, so an empty view is what hides it.
            _failure = result.unsupported ? "Unsupported file format" : Hud::failure_text(*_folder, result.error);
            _loading = false;
            _shownGeneration = result.generation;
            _viewport->set_image(0, 0);
            *_dirty = true;

            continue;
        }

        // Nothing to draw until the tiles are made, so the old image goes and the view
        // says how far they are.
        if (result.kind == Loader::Kind::Building) {
            _shownGeneration = result.generation;
            _info = result.info;
            _turn = 1;
            _building = true;
            _tileFolder = result.tileFolder;
            _loading = true;
            _window->canvas().clear();
            _viewport->set_image(0, 0);
            *_dirty = true;

            continue;
        }

        // The first result of a new image replaces the old one whole.
        if (_shownGeneration != result.generation || _building) {
            _shownGeneration = result.generation;
            _info = result.info;
            _turn = 1;
            _building = false;

            const Size size = shown();

            _viewport->set_image(size.width, size.height);
            moved();
        }

        if (result.animation != nullptr && result.animation == _playback->animation()) {
            continue;
        }

        const Size size = stored();
        const auto image = static_cast<std::uint64_t>(_folder->index());

        _streamed = result.tileCache != nullptr;
        _stale = _streamed && result.tileCache->stale();
        _window->follow_output(result);

        if (_streamed) {
            // Each tile read wakes the loop, which uploads it.
            result.tileCache->on_ready([event = _tileCacheEvent] {
                SDL_Event ready{};

                ready.type = event;
                SDL_PushEvent(&ready);
            });

            _window->canvas().show(result.tileCache, image, size.width, size.height, orientation());
        } else {
            _window->canvas().show(result.pyramid, image, size.width, size.height, orientation());
        }

        if (result.animation != nullptr) {
            _playback->start(result.animation, SDL_GetTicks());
            _window->canvas().replace(_playback->pyramid());
        }
        // Straight away, or the frame between the old image and this one is blank.
        _window->canvas().upload(*_viewport, App::UPLOAD_BUDGET);

        switch (result.kind) {
            case Loader::Kind::Preview:
                _loading = true;
                break;
            case Loader::Kind::Full:
                // An animation is still loading until its frames arrive.
                _loading = result.info.frames > 1 && result.animation == nullptr;
                break;
            case Loader::Kind::Building:
            case Loader::Kind::Failed:
                break;
        }

        *_dirty = true;
    }

    warm_neighbours();
    _freedSinceGiveBack = true;
}

// Once everything has arrived and navigation rests, what the old images held goes back to the system.
void Picture::give_back() {
    if (_freedSinceGiveBack && !_loading
        && SDL_GetTicks() - _shownAt >= static_cast<std::uint64_t>(Loader::GIVE_BACK_MS) && _loader->idle()) {
        Memory::give_back();
        _freedSinceGiveBack = false;
    }
}

void Picture::deliver_detail() {
    Refiner::Result result;

    while (_refiner->take(&result)) {
        if (result.generation == _generation && _shownGeneration == _generation) {
            _window->canvas().refine(std::move(result.bitmap), result.scale, result.x, result.y);
            *_dirty = true;
        }
    }
}

void Picture::moved() {
    _movedAt = SDL_GetTicks();
    _settling = true;
}

// Once the view rests on a scalable image, the part on screen renders again at the zoom shown.
void Picture::refine() {
    if (!_settling || SDL_GetTicks() - _movedAt < static_cast<std::uint64_t>(Refiner::REST_MS)) {
        return;
    }

    _settling = false;

    if (!Decode::scalable(_info.kind) || _shownGeneration != _generation || !_viewport->has_image()) {
        return;
    }

    const Rect image = _viewport->image_rect();
    const double left = std::max(image.x, 0.0);
    const double top = std::max(image.y, 0.0);
    const double right = std::min(image.x + image.width, _viewport->area_width());
    const double bottom = std::min(image.y + image.height, _viewport->area_height());

    if (right <= left || bottom <= top) {
        return;
    }

    const double zoom = _viewport->zoom();
    const Size size = stored();
    const int back = Orient::inverse(orientation());

    // A rect on screen as a share of the image as stored, within the image.
    const auto unit = [&](const double x0, const double y0, const double x1, const double y1) {
        const double cx0 = std::clamp((x0 - image.x) / image.width, 0.0, 1.0);
        const double cy0 = std::clamp((y0 - image.y) / image.height, 0.0, 1.0);
        const double cx1 = std::clamp((x1 - image.x) / image.width, 0.0, 1.0);
        const double cy1 = std::clamp((y1 - image.y) / image.height, 0.0, 1.0);

        return oriented({.x = cx0, .y = cy0, .width = cx1 - cx0, .height = cy1 - cy0}, back);
    };

    const Rect seen = unit(left, top, right, bottom);
    const Rect seenPixels{
            .x = seen.x * size.width,
            .y = seen.y * size.height,
            .width = seen.width * size.width,
            .height = seen.height * size.height,
    };

    if (_window->canvas().refined(zoom, seenPixels)) {
        return;
    }

    const double marginX = _viewport->area_width() * REFINE_MARGIN;
    const double marginY = _viewport->area_height() * REFINE_MARGIN;
    const Rect wide = unit(left - marginX, top - marginY, right + marginX, bottom + marginY);
    const double scaledWidth = size.width * zoom;
    const double scaledHeight = size.height * zoom;
    const auto x = static_cast<int>(std::floor(wide.x * scaledWidth));
    const auto y = static_cast<int>(std::floor(wide.y * scaledHeight));
    const Ask ask{
            .generation = _generation,
            .scale = zoom,
            .x = x,
            .y = y,
            .width = static_cast<int>(std::ceil((wide.x + wide.width) * scaledWidth)) - x,
            .height = static_cast<int>(std::ceil((wide.y + wide.height) * scaledHeight)) - y,
    };

    if (ask == _asked) {
        return;
    }

    _asked = ask;
    _refiner->render(ask.generation, _folder->current(), ask.scale, ask.x, ask.y, ask.width, ask.height);
}

// The images either side stay on the GPU, so stepping to them draws at once.
void Picture::warm_neighbours() {
    if (_footprint) {
        // The image before a failed one would otherwise stay on the canvas, hidden.
        if (!_failure.empty()) {
            _window->canvas().clear();
        }

        _window->canvas().keep({});

        return;
    }

    if (_folder->count() < 2) {
        return;
    }

    std::vector<std::uint64_t> keep;

    for (const int step : {_direction, -_direction}) {
        const int index = _folder->wrap(_folder->index() + step);

        if (index == _folder->index()) {
            continue;
        }

        keep.push_back(static_cast<std::uint64_t>(index));

        Decode::Info info;
        std::shared_ptr<const Pyramid> pyramid;

        if (_loader->cached(_folder->at(index), &info, &pyramid) && pyramid != nullptr) {
            const bool swapped = Orient::swaps(info.orientation);

            _window->canvas().warm(std::move(pyramid),
                                   static_cast<std::uint64_t>(index),
                                   swapped ? info.height : info.width,
                                   swapped ? info.width : info.height,
                                   info.orientation);
        }
    }

    _window->canvas().keep(keep);
}

// Tiles of the same image and size would be kept, so the canvas lets go of them first.
void Picture::reload() {
    if (_folder->count() == 0) {
        return;
    }

    _loader->forget(_folder->current());
    _window->canvas().clear();
    show(_folder->index(), _direction);
}

void Picture::rebuild() {
    if (_folder->count() == 0) {
        return;
    }

    if (!_streamed && !_building) {
        _flash = "Not streamed, no tile cache to rebuild";
        _switchedAt = SDL_GetTicks();
        *_dirty = true;

        return;
    }

    _loader->rebuild(_folder->current());
    _window->canvas().clear();
    show(_folder->index(), _direction);
}

void Picture::turn(const int by) {
    if (!_viewport->has_image()) {
        return;
    }

    _turn = Orient::compose(by, _turn);
    _message.clear();
    _window->canvas().orient(orientation());

    const Size size = shown();

    _viewport->set_image(size.width, size.height);
    moved();
    *_dirty = true;
}

void Picture::toggle_stream() {
    const bool on = !_loader->streaming_all();

    _switchedAt = SDL_GetTicks();
    *_dirty = true;
    _loader->stream_all(on);

    // It streams whatever the mode, so it stays on screen and only the neighbours decode the new way.
    if (always_streamed()) {
        _flash = "Too large for memory, always streamed";
        _window->canvas().keep({});

        return;
    }

    _flash = on ? "Streaming mode on" : "Streaming mode off";
    // Tiles of the same image and size would be kept, so the canvas lets go of them first.
    _window->canvas().clear();

    if (_folder->count() == 0) {
        return;
    }

    show(_folder->index(), _direction);
}

// The images prefetched stay as they are, and the next step fetches the new neighbours. The canvas
// keeps its images by place in the folder, so the image on screen takes its new place. Before the
// current image arrives, or once it failed, the canvas holds the one before, which goes.
void Picture::next_order() {
    const Hud::OrderText &next = Hud::next_order(_folder->order());

    _folder->sort(next.order);

    if (_shownGeneration == _generation && _failure.empty()) {
        _window->canvas().renumber(static_cast<std::uint64_t>(_folder->index()));
    } else {
        _window->canvas().clear();
    }

    _flash = next.text;
    _switchedAt = SDL_GetTicks();
    *_dirty = true;
}

bool Picture::settled() const {
    // Only a scalable image renders again once the view rests. For any other image the rest does not matter.
    const bool resting = _settling && Decode::scalable(_info.kind);

    return _shownGeneration == _generation && !_loading && !_building && !resting;
}

std::uint64_t Picture::rests_at() const {
    return _movedAt + static_cast<std::uint64_t>(Refiner::REST_MS);
}

std::uint64_t Picture::give_back_at() const {
    return _shownAt + static_cast<std::uint64_t>(Loader::GIVE_BACK_MS);
}

bool Picture::always_streamed() const {
    return (_streamed || _building) && static_cast<std::int64_t>(_info.width) * _info.height > Loader::max_pixels();
}

void Picture::save() {
    if (_turn == 1 || _shownGeneration != _generation || _info.width == 0) {
        return;
    }

    *_dirty = true;

    if (!Reorient::supported(_info.kind)) {
        _message = std::format("{} files keep no orientation, not saved", _info.format);

        return;
    }

    const int target = orientation();
    std::string error;

    if (!Reorient::write(_folder->current(), target, &error)) {
        std::println(stderr, "tinyiv: {}", error);
        _message = "Could not save";

        return;
    }

    _loader->reoriented(_folder->current(), target);

    if (Orient::swaps(_turn)) {
        std::swap(_info.width, _info.height);
    }

    std::error_code failure;

    _info.orientation = target;
    _turn = 1;
    _bytes = std::filesystem::file_size(_folder->current(), failure);
    _message = "Saved";
}

int Picture::orientation() const {
    return Orient::compose(_turn, _info.orientation);
}

Picture::Size Picture::stored() const {
    return Orient::swaps(_info.orientation) ? Size{.width = _info.height, .height = _info.width}
                                            : Size{.width = _info.width, .height = _info.height};
}

Picture::Size Picture::shown() const {
    return Orient::swaps(_turn) ? Size{.width = _info.height, .height = _info.width}
                                : Size{.width = _info.width, .height = _info.height};
}
}
