// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "app/App.h"
#include "app/Config.h"
#include "app/Hud.h"
#include "app/Input.h"
#include "app/Picture.h"
#include "app/Window.h"
#include "gallery/Folder.h"
#include "image/Orient.h"
#include "image/decode/Decode.h"
#include "render/Canvas.h"
#include "render/Painter.h"
#include "render/PlayBar.h"
#include "render/StatusBar.h"
#include "services/Loader.h"
#include "services/Memory.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv {
namespace {
constexpr Colour BACKGROUND{.r = 0, .g = 0, .b = 0, .a = 255};

bool is_window_event(const SDL_Event &event) {
    return event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST;
}
}

App::~App() {
    _refiner = nullptr;
    _loader = nullptr;
}

bool App::start(const std::filesystem::path &file, std::string *error) {
    std::vector<std::string> warnings;
    const Config config = Config::load(Config::location(), &warnings);

    for (const std::string &warning : warnings) {
        std::println(stderr, "tinyiv: {}", warning);
    }

    if (config.footprint) {
        Memory::lean();
    }

    _folder.sort(config.order);

    if (!file.empty() && !_folder.open(file, Decode::suffixes(), error)) {
        return false;
    }

    // The first decode starts before the window exists, so the two overlap.
    _loaderEvent = SDL_RegisterEvents(3);
    _refinerEvent = _loaderEvent + 1;
    _tileCacheEvent = _loaderEvent + 2;
    _loader = std::make_unique<Loader>(_loaderEvent, 0, 0, config.footprint);

    _loader->set_tiles(config.cache, config.small, config.persist);
    _loader->stream_all(config.streaming);
    _refiner = std::make_unique<Refiner>(_refinerEvent);
    _picture.attach({
            .folder = &_folder,
            .viewport = &_viewport,
            .playback = &_playback,
            .window = &_window,
            .loader = _loader.get(),
            .refiner = _refiner.get(),
            .dirty = &_dirty,
            .seeking = &_seeking,
            .tileCacheEvent = _tileCacheEvent,
            .footprint = config.footprint,
    });
    _picture.show(_folder.index(), 1);

    if (!_window.open(config, error)) {
        return false;
    }

    if (const std::optional<Window::Size> desktop = _window.desktop_pixels()) {
        _loader->set_screen(desktop->width, desktop->height);
    }

    layout();
    update_display();
    _window.update_refresh();
    // Said even when SDR, since HDR images wait for it before they build pyramids of tiles.
    _loader->set_display(_window.display());
    // A result posted before SDL_Init has no event queue to wake the loop.
    deliver();

    return true;
}

void App::layout() {
    const Window::Size size = _window.pixels();

    _viewport.set_area(size.width, size.height - (2 * bar_height()));
    _repaints = REPAINTS;
    _picture.moved();
}

bool App::bar_shown() const {
    return _window.fullscreen() ? _barFullscreen : _barWindowed;
}

int App::bar_height() const {
    return bar_shown() ? StatusBar::height(_window.scale()) : 0;
}

void App::toggle_bar() {
    bool &shown = _window.fullscreen() ? _barFullscreen : _barWindowed;

    shown = !shown;
    layout();
    _dirty = true;
}

void App::deliver() {
    _picture.deliver();

    if (_window.painter() == nullptr) {
        _running = false;
    }
}

void App::run() {
    while (_running) {
        // Asks for the streamed tiles the view meets before deciding there is nothing to do.
        _window.canvas().fetch(_viewport);

        // Nothing to upload and no frame to draw yet means nothing to do until something happens.
        // The exceptions are a frame coming due, a loading indicator that has to keep moving and
        // an animation with frames due.
        if (!_window.canvas().pending(_viewport) && frame_wait_ms() != 0) {
            wait_for_event();
        }

        SDL_Event event{};

        while (_running && SDL_PollEvent(&event)) {
            handle(event);
        }

        if (!_running) {
            break;
        }

        if (_playback.advance(SDL_GetTicks())) {
            _window.canvas().replace(_playback.pyramid());
            _dirty = true;
        }

        _picture.refine();

        if (_window.canvas().pending(_viewport)) {
            _window.canvas().upload(_viewport, UPLOAD_BUDGET);
            _dirty = true;
        }

        if (frame_wait_ms() == 0) {
            _drawnAt = SDL_GetTicksNS();
            frame();
            // Presented from idle, a frame can land before the compositor has caught up and
            // stay unseen until the next, so every change is drawn once more after it.
            _repaints = _dirty ? std::max(_repaints - 1, 1) : std::max(_repaints - 1, 0);
            _dirty = false;
        }
    }
}

App::State App::state() const {
    const bool settled = _picture.settled() && !_dirty && _repaints == 0 && !_window.canvas().pending(_viewport)
                         && !_window.canvas().reading() && _loader->idle() && _refiner->idle();

    return {
            .file = _folder.count() > 0 ? _folder.current() : std::filesystem::path(),
            .index = _folder.index(),
            .count = _folder.count(),
            .order = _folder.order(),
            .info = _picture.info(),
            .orientation = _picture.orientation(),
            .settled = settled,
            .failed = !_picture.failure().empty(),
            .streamed = _picture.streamed(),
            .streaming = _loader->streaming_all(),
            .hdr10 = _window.hdr10(),
            .animated = _playback.active(),
            .playing = _playback.playing(),
            .frame = _playback.frame(),
            .zoom = _viewport.zoom(),
            .progress = _loader->progress(),
            .help = _help,
            .fullscreen = _window.fullscreen(),
            .bars = bar_shown(),
            .message = _picture.message(),
    };
}

void App::handle(const SDL_Event &event) {
    if (event.type == _loaderEvent) {
        deliver();

        return;
    }

    if (event.type == _refinerEvent) {
        _picture.deliver_detail();

        return;
    }

    // The loop uploads it next.
    if (event.type == _tileCacheEvent) {
        _dirty = true;

        return;
    }

    // SDL follows the same colour feedback as the Wayland module and raises a window event when it changes.
    if (is_window_event(event)) {
        layout();
        update_display();
        _window.update_refresh();

        return;
    }

    if (handle_help(event) || handle_play_bar(event)) {
        return;
    }

    if (event.type == SDL_EVENT_DROP_FILE) {
        std::string error;

        if (_folder.open(event.drop.data, Decode::suffixes(), &error)) {
            _window.canvas().clear();
            _picture.show(_folder.index(), 1);
        } else {
            std::println(stderr, "tinyiv: {}", error);
        }

        return;
    }

    const Input::Action action = _input.handle(event, _window.density(), bar_height(), _viewport);

    // Doing anything the help lists puts the help away.
    if (_help && action != Input::Action::None && action != Input::Action::ToggleHelp) {
        _help = false;
        _dirty = true;
    }

    switch (action) {
        case Input::Action::Quit:
            _running = false;
            break;
        case Input::Action::ToggleFullscreen:
            _window.set_fullscreen(!_window.fullscreen());
            layout();
            break;
        case Input::Action::Scroll: {
            const int steps = _input.scroll();

            _picture.show(_folder.index() + steps, steps > 0 ? 1 : -1);
            break;
        }
        case Input::Action::First:
            _picture.show(0, 1);
            break;
        case Input::Action::Last:
            _picture.show(_folder.count() - 1, -1);
            break;
        case Input::Action::Reload:
            _picture.reload();
            break;
        case Input::Action::Rebuild:
            _picture.rebuild();
            break;
        case Input::Action::TurnLeft:
            _picture.turn(Orient::TURN_LEFT);
            break;
        case Input::Action::TurnRight:
            _picture.turn(Orient::TURN_RIGHT);
            break;
        case Input::Action::FlipVertical:
            _picture.turn(Orient::FLIP_VERTICAL);
            break;
        case Input::Action::FlipHorizontal:
            _picture.turn(Orient::FLIP_HORIZONTAL);
            break;
        case Input::Action::Save:
            _picture.save();
            break;
        case Input::Action::ToggleStream:
            _picture.toggle_stream();
            break;
        case Input::Action::NextOrder:
            _picture.next_order();
            break;
        case Input::Action::ToggleBar:
            toggle_bar();
            break;
        case Input::Action::ToggleHelp:
            _help = !_help;
            _dirty = true;
            break;
        case Input::Action::TogglePlay:
            _playback.toggle(SDL_GetTicks());
            _dirty = true;
            break;
        case Input::Action::FrameBack:
            step_frame(-1);
            break;
        case Input::Action::FrameForward:
            step_frame(1);
            break;
        case Input::Action::Redraw:
            _dirty = true;
            _picture.moved();
            break;
        case Input::Action::None:
            break;
    }
}

// While the help is up, Escape and clicks outside it put it away instead of acting, and
// clicks inside it do nothing. True when the event went to it.
bool App::handle_help(const SDL_Event &event) {
    if (!_help) {
        return false;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        _help = false;
        _dirty = true;

        return true;
    }

    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN) {
        return false;
    }

    const Window::Size size = _window.pixels();
    const float density = _window.density();
    const Rect box = Hud::help_box(
            {.x = 0.0, .y = 0.0, .width = static_cast<double>(size.width), .height = static_cast<double>(size.height)},
            _window.scale());
    const double x = event.button.x * density;
    const double y = event.button.y * density;

    if (x < box.x || y < box.y || x > box.x + box.width || y > box.y + box.height) {
        _help = false;
        _dirty = true;
    }

    return true;
}

// Clicks and drags on the play bar, which the view never sees. True when the event was one.
bool App::handle_play_bar(const SDL_Event &event) {
    if (!_playback.active()) {
        return false;
    }

    const float density = _window.density();

    switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (event.button.button != SDL_BUTTON_LEFT) {
                return false;
            }

            const double x = event.button.x * density;

            switch (PlayBar::hit(play_bar(), _window.scale(), x, event.button.y * density)) {
                case PlayBar::Part::Toggle:
                    _playback.toggle(SDL_GetTicks());
                    _dirty = true;
                    break;
                case PlayBar::Part::Track:
                    _seeking = true;
                    seek_to(x);
                    break;
                case PlayBar::Part::None:
                    return false;
            }

            return true;
        }

        case SDL_EVENT_MOUSE_MOTION:
            if (_seeking) {
                seek_to(event.motion.x * density);
            }

            return _seeking;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!_seeking || event.button.button != SDL_BUTTON_LEFT) {
                return false;
            }

            _seeking = false;

            return true;

        default:
            return false;
    }
}

void App::seek_to(const double x) {
    _playback.seek(PlayBar::seek(play_bar(), _window.scale(), x), SDL_GetTicks());
    _window.canvas().replace(_playback.pyramid());
    _dirty = true;
}

void App::step_frame(const int delta) {
    if (!_playback.active()) {
        return;
    }

    _playback.step(delta);
    _window.canvas().replace(_playback.pyramid());
    _dirty = true;
}

void App::update_display() {
    if (!_window.update_display()) {
        return;
    }

    _loader->set_display(_window.display());

    if (_picture.info().hdr && _folder.count() > 0) {
        _window.canvas().clear();
        _picture.show(_folder.index(), _picture.direction());
    }
}

void App::frame() {
    const Window::Size size = _window.pixels();
    const float scale = _window.scale();
    Painter *painter = _window.painter();

    if (painter == nullptr) {
        std::println(stderr, "tinyiv: drawing stopped, the GPU failed or was lost");

        _running = false;

        return;
    }

    if (!painter->begin_frame(BACKGROUND)) {
        return;
    }

    const int bar = bar_height();

    if (_viewport.has_image()) {
        const Rect below{
                .x = 0.0,
                .y = static_cast<double>(bar),
                .width = static_cast<double>(size.width),
                .height = static_cast<double>(size.height - (2 * bar)),
        };

        painter->set_view(&below);
        _window.canvas().draw(_viewport);
        painter->set_view(nullptr);
    }

    Hud::draw(*painter,
              {
                      .width = size.width,
                      .height = size.height,
                      .scale = scale,
                      .bar = bar,
                      .bars = bar_shown(),
                      .help = _help,
                      .hdr10 = _window.hdr10(),
                      .streaming = _loader->streaming_all(),
                      .progress = _loader->progress(),
                      .folder = &_folder,
                      .viewport = &_viewport,
                      .playback = &_playback,
                      .picture = &_picture,
              });

    painter->end_frame();
}

void App::wait_for_event() {
    _picture.give_back();

    SDL_Event event{};
    const int deadline = wait_ms();
    const int frameWait = frame_wait_ms();
    const int wait = deadline < 0 || (frameWait >= 0 && frameWait < deadline) ? frameWait : deadline;

    if (wait < 0 ? SDL_WaitEvent(&event) : SDL_WaitEventTimeout(&event, wait)) {
        handle(event);
    } else if (wait == deadline) {
        // A deadline changes the frame, such as the badge going away. A frame coming due does not.
        _dirty = true;
    }
}

int App::frame_wait_ms() const {
    if (!_dirty && _repaints == 0) {
        return -1;
    }

    // Only a drag asks for frames faster than the display shows them. Anything else is drawn at once.
    if (_window.vsync() || (!_input.dragging() && !_seeking)) {
        return 0;
    }

    const auto gap = static_cast<std::uint64_t>(static_cast<double>(SDL_NS_PER_SECOND) / 2.0 / _window.refresh_rate());
    const std::uint64_t due = _drawnAt + gap;
    const std::uint64_t now = SDL_GetTicksNS();

    // Rounded up, so a wait shorter than a millisecond sleeps instead of spinning.
    return due > now ? static_cast<int>((due - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS) : 0;
}

int App::wait_ms() const {
    const std::uint64_t now = SDL_GetTicks();
    int wait = _picture.loading() ? StatusBar::LOADING_TICK_MS : -1;

    const auto until = [&](const std::uint64_t at) {
        const int left = at > now ? static_cast<int>(at - now) : 0;

        wait = wait < 0 ? left : std::min(wait, left);
    };

    if (_playback.playing()) {
        until(_playback.due());
    }

    if (_picture.settling()) {
        until(_picture.rests_at());
    }

    // Wakes to take the badge away again.
    if (Hud::flashing(_picture.switched_at())) {
        until(_picture.switched_at() + Hud::MODE_FLASH_MS);
    }

    // Past it, the next event or delivery gives memory back, and waking early would only spin.
    if (const std::uint64_t due = _picture.give_back_at(); _picture.freed_since_give_back() && due > now) {
        until(due);
    }

    return wait;
}

Rect App::play_bar() const {
    return Hud::play_bar(_viewport, _window.scale(), bar_height());
}
}
