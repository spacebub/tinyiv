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
#include "app/Input.h"
#include "gallery/Folder.h"
#include "image/Decode.h"
#include "render/Canvas.h"
#include "render/PlayBar.h"
#include "render/StatusBar.h"
#include "services/Loader.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        constexpr float WINDOW_SHARE = 0.8F;
        constexpr int PREFETCH_AHEAD = 6;
        constexpr int PREFETCH_BEHIND = 2;
        constexpr double BADGE_MARGIN = 12.0;
        // A scalable image renders this share of the view beyond each edge, so a short pan stays sharp.
        constexpr double REFINE_MARGIN = 0.25;
        constexpr SDL_Color BACKGROUND{0, 0, 0, 255};

        std::string human_size(const std::uintmax_t bytes) {
            constexpr double KIB = 1024.0;
            constexpr double MIB = KIB * 1024.0;
            const auto amount = static_cast<double>(bytes);

            if (amount >= MIB) {
                return std::format("{:.1f} MiB", amount / MIB);
            }

            return std::format("{:.0f} KiB", amount / KIB);
        }

        int scaled(const int value, const float by) {
            return static_cast<int>(static_cast<float>(value) * by);
        }

        bool is_window_event(const SDL_Event &event) {
            return event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST;
        }

        // Zero is mailbox on the gpu renderer, so each refresh shows the newest frame. Under FIFO a
        // drag that starts from idle queues frames ahead of the display and stutters until it fills.
        int vsync_setting(SDL_Renderer *renderer, SDL_Window *window) {
            auto *device = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));

            return device != nullptr && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX) ? 0 : 1;
        }
    }

    App::~App() {
        _refiner = nullptr;
        _loader = nullptr;
        _canvas = nullptr;

        if (_renderer != nullptr) {
            SDL_DestroyRenderer(_renderer);
        }

        if (_window != nullptr) {
            SDL_DestroyWindow(_window);
        }

        SDL_Quit();
    }

    bool App::start(const std::filesystem::path &file, std::string *error) {
        if (!_folder.open(file, Decode::suffixes(), error)) {
            return false;
        }

        // The first decode starts before the window exists, so the two overlap.
        _loaderEvent = SDL_RegisterEvents(2);
        _refinerEvent = _loaderEvent + 1;
        _loader = std::make_unique<Loader>(_loaderEvent);
        _refiner = std::make_unique<Refiner>(_refinerEvent);

        show(_folder.index(), 1);

        if (!open_window(error)) {
            return false;
        }

        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(_window));

        if (desktop != nullptr) {
            _loader->set_screen(scaled(desktop->w, desktop->pixel_density), scaled(desktop->h, desktop->pixel_density));
        }

        layout();
        // A result posted before SDL_Init has no event queue to wake the loop.
        deliver();

        return true;
    }

    bool App::open_window(std::string *error) {
        // The identifier is the window's app id, which is how the desktop finds the icon.
        SDL_SetAppMetadata("tinyiv", "0.1.0", "tinyiv");

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            *error = SDL_GetError();

            return false;
        }

        SDL_Rect usable{0, 0, 1280, 800};

        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);

        _window = SDL_CreateWindow("tinyiv", scaled(usable.w, WINDOW_SHARE), scaled(usable.h, WINDOW_SHARE), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

        if (_window == nullptr) {
            *error = SDL_GetError();

            return false;
        }

        _renderer = SDL_CreateRenderer(_window, "gpu");

        if (_renderer == nullptr) {
            _renderer = SDL_CreateRenderer(_window, nullptr);
        }

        if (_renderer == nullptr) {
            *error = SDL_GetError();

            return false;
        }

        SDL_SetRenderVSync(_renderer, vsync_setting(_renderer, _window));

        const auto maxTexture = static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(_renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, Tiles::SIZE));
        _canvas = std::make_unique<Canvas>(_renderer, maxTexture);

        return true;
    }

    void App::layout() {
        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(_window, &width, &height);

        const int bar = _fullscreen ? 0 : StatusBar::height(SDL_GetWindowDisplayScale(_window));

        _viewport.set_area(width, height - bar);
        _repaints = REPAINTS;
        moved();
    }

    void App::set_fullscreen(const bool on) {
        _fullscreen = on;

        SDL_SetWindowFullscreen(_window, on);
        layout();
    }

    void App::show(const int index, const int direction) {
        _folder.step(_folder.wrap(index) - _folder.index());
        _direction = direction;

        std::error_code failure;

        _bytes = std::filesystem::file_size(_folder.current(), failure);
        _info = {};
        _loading = true;
        _failure.clear();
        _seeking = false;
        _playback.stop();
        _refiner->cancel();
        _asked = {};

        std::vector<std::filesystem::path> ahead;
        std::vector<std::filesystem::path> behind;
        const int count = _folder.count();

        for (int i = 1; i <= PREFETCH_AHEAD && i < count; ++i) {
            ahead.push_back(_folder.at(_folder.index() + (i * direction)));
        }

        for (int i = 1; i <= PREFETCH_BEHIND && i + PREFETCH_AHEAD < count; ++i) {
            behind.push_back(_folder.at(_folder.index() - (i * direction)));
        }

        _loader->show(++_generation, _folder.current(), std::move(ahead), std::move(behind));
        _dirty = true;
    }

    void App::run() {
        while (_running) {
            SDL_Event event{};
            const bool idle = !_canvas->pending(_viewport) && _repaints == 0;

            // Nothing to upload or repaint means nothing to do until something happens, except
            // that a loading indicator has to keep moving and an animation has frames due.
            if (idle) {
                const int wait = wait_ms();

                if (wait < 0 ? SDL_WaitEvent(&event) : SDL_WaitEventTimeout(&event, wait)) {
                    handle(event);
                } else if (_loading) {
                    _dirty = true;
                }
            }

            while (_running && SDL_PollEvent(&event)) {
                handle(event);
            }

            if (!_running) {
                break;
            }

            if (_playback.advance(SDL_GetTicks())) {
                _canvas->replace(_playback.pyramid());
                _dirty = true;
            }

            refine();

            if (_canvas->pending(_viewport)) {
                _canvas->upload(_viewport, UPLOAD_BUDGET);
                _dirty = true;
            }

            if (_dirty || _repaints > 0) {
                frame();
                _dirty = false;
                _repaints = std::max(_repaints - 1, 0);
            }
        }
    }

    void App::handle(const SDL_Event &event) {
        if (event.type == _loaderEvent) {
            deliver();

            return;
        }

        if (event.type == _refinerEvent) {
            deliver_detail();

            return;
        }

        if (is_window_event(event)) {
            layout();

            return;
        }

        if (handle_play_bar(event)) {
            return;
        }

        if (event.type == SDL_EVENT_DROP_FILE) {
            std::string error;

            if (_folder.open(event.drop.data, Decode::suffixes(), &error)) {
                _canvas->clear();
                show(_folder.index(), 1);
            } else {
                std::println(stderr, "tinyiv: {}", error);
            }

            return;
        }

        switch (_input.handle(event, SDL_GetWindowPixelDensity(_window), _viewport)) {
            case Input::Action::Quit:
                _running = false;
                break;
            case Input::Action::ToggleFullscreen:
                set_fullscreen(!_fullscreen);
                break;
            case Input::Action::Scroll: {
                const int steps = _input.scroll();

                show(_folder.index() + steps, steps > 0 ? 1 : -1);
                break;
            }
            case Input::Action::First:
                show(0, 1);
                break;
            case Input::Action::Last:
                show(_folder.count() - 1, -1);
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
                moved();
                break;
            case Input::Action::None:
                break;
        }
    }

    // Clicks and drags on the play bar, which the view never sees. True when the event was one.
    bool App::handle_play_bar(const SDL_Event &event) {
        if (!_playback.active()) {
            return false;
        }

        const float density = SDL_GetWindowPixelDensity(_window);

        switch (event.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (event.button.button != SDL_BUTTON_LEFT) {
                    return false;
                }

                const double x = event.button.x * density;

                switch (PlayBar::hit(play_bar(), SDL_GetWindowDisplayScale(_window), x, event.button.y * density)) {
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
        _playback.seek(PlayBar::seek(play_bar(), SDL_GetWindowDisplayScale(_window), x), SDL_GetTicks());
        _canvas->replace(_playback.pyramid());
        _dirty = true;
    }

    void App::step_frame(const int delta) {
        if (!_playback.active()) {
            return;
        }

        _playback.step(delta);
        _canvas->replace(_playback.pyramid());
        _dirty = true;
    }

    void App::deliver() {
        Loader::Result result;

        while (_loader->take(&result)) {
            if (result.generation != _generation) {
                continue;
            }

            if (result.kind == Loader::Kind::Failed) {
                std::println(stderr, "tinyiv: {}", result.error);

                // The previous image stays on the canvas, so an empty view is what hides it.
                _failure = result.unsupported ? "Unsupported file format" : "Could not open this image";
                _loading = false;
                _shown = result.generation;
                _viewport.set_image(0, 0);
                _dirty = true;

                continue;
            }

            // The first result of a new image replaces the old one whole.
            if (_shown != result.generation) {
                _shown = result.generation;
                _info = result.info;
                _viewport.set_image(_info.width, _info.height);
                moved();
            }

            if (result.animation != nullptr && result.animation == _playback.animation()) {
                continue;
            }

            _canvas->show(result.pyramid, static_cast<std::uint64_t>(_folder.index()), _info.width, _info.height);

            if (result.animation != nullptr) {
                _playback.start(result.animation, SDL_GetTicks());
                _canvas->replace(_playback.pyramid());
            }
            // Straight away, or the frame between the old image and this one is blank.
            _canvas->upload(_viewport, UPLOAD_BUDGET);

            switch (result.kind) {
                case Loader::Kind::Preview:
                    _loading = true;
                    break;
                case Loader::Kind::Full:
                    // An animation is still loading until its frames arrive.
                    _loading = result.info.frames > 1 && result.animation == nullptr;
                    break;
                case Loader::Kind::Failed:
                    break;
            }

            _dirty = true;
        }

        warm_neighbours();
    }

    void App::deliver_detail() {
        Refiner::Result result;

        while (_refiner->take(&result)) {
            if (result.generation == _generation && _shown == _generation) {
                _canvas->refine(std::move(result.bitmap), result.scale, result.x, result.y);
                _dirty = true;
            }
        }
    }

    void App::moved() {
        _movedAt = SDL_GetTicks();
        _settling = true;
    }

    // Once the view rests on a scalable image, the part on screen renders again at the zoom shown.
    void App::refine() {
        if (!_settling || SDL_GetTicks() - _movedAt < static_cast<std::uint64_t>(Refiner::REST_MS)) {
            return;
        }

        _settling = false;

        if (!Decode::scalable(_info.kind) || _shown != _generation || !_viewport.has_image()) {
            return;
        }

        // In pixels of the image at the zoom.
        const Rect image = _viewport.image_rect();
        const double left = std::max(-image.x, 0.0);
        const double top = std::max(-image.y, 0.0);
        const double right = std::min(_viewport.area_width() - image.x, image.width);
        const double bottom = std::min(_viewport.area_height() - image.y, image.height);

        if (right <= left || bottom <= top) {
            return;
        }

        const double zoom = _viewport.zoom();

        if (_canvas->refined(zoom, {left / zoom, top / zoom, (right - left) / zoom, (bottom - top) / zoom})) {
            return;
        }

        const double marginX = _viewport.area_width() * REFINE_MARGIN;
        const double marginY = _viewport.area_height() * REFINE_MARGIN;
        const auto x = static_cast<int>(std::floor(std::max(left - marginX, 0.0)));
        const auto y = static_cast<int>(std::floor(std::max(top - marginY, 0.0)));
        const Ask ask{
                _generation,
                zoom,
                x,
                y,
                static_cast<int>(std::ceil(std::min(right + marginX, image.width))) - x,
                static_cast<int>(std::ceil(std::min(bottom + marginY, image.height))) - y,
        };

        if (ask == _asked) {
            return;
        }

        _asked = ask;
        _refiner->render(ask.generation, _folder.current(), ask.scale, ask.x, ask.y, ask.width, ask.height);
    }

    // The images either side stay on the GPU, so stepping to them draws at once.
    void App::warm_neighbours() {
        if (_folder.count() < 2) {
            return;
        }

        std::vector<std::uint64_t> keep;

        for (const int step : {_direction, -_direction}) {
            const int index = _folder.wrap(_folder.index() + step);

            if (index == _folder.index()) {
                continue;
            }

            keep.push_back(static_cast<std::uint64_t>(index));

            Decode::Info info;
            std::shared_ptr<const Pyramid> pyramid;

            if (_loader->cached(_folder.at(index), &info, &pyramid) && pyramid != nullptr) {
                _canvas->warm(std::move(pyramid), static_cast<std::uint64_t>(index), info.width, info.height);
            }
        }

        _canvas->keep(keep);
    }

    void App::frame() {
        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(_window, &width, &height);

        const float scale = SDL_GetWindowDisplayScale(_window);

        SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(_renderer, BACKGROUND.r, BACKGROUND.g, BACKGROUND.b, BACKGROUND.a);
        SDL_RenderClear(_renderer);

        if (_viewport.has_image()) {
            _canvas->draw(_viewport);
        } else if (!_failure.empty()) {
            StatusBar::notice(_renderer, {0.0, 0.0, _viewport.area_width(), _viewport.area_height()}, scale, _failure);
        }

        if (_playback.active()) {
            PlayBar::draw(_renderer, play_bar(), scale, _playback.playing(), _playback.progress());
        }

        if (!_fullscreen) {
            const int bar = StatusBar::height(scale);

            StatusBar::draw(_renderer, {0.0, static_cast<double>(height - bar), static_cast<double>(width), static_cast<double>(bar)}, scale, bar_left(), bar_right(), _loading);
        } else if (_loading) {
            // Where the bar would be, for as long as something is still on its way.
            StatusBar::badge(_renderer, BADGE_MARGIN * scale, height - (BADGE_MARGIN * scale), scale, bar_left(), true);
        }

        SDL_RenderPresent(_renderer);
    }

    int App::wait_ms() const {
        const std::uint64_t now = SDL_GetTicks();
        int wait = _loading ? StatusBar::LOADING_TICK_MS : -1;

        const auto until = [&](const std::uint64_t at) {
            const int left = at > now ? static_cast<int>(at - now) : 0;

            wait = wait < 0 ? left : std::min(wait, left);
        };

        if (_playback.playing()) {
            until(_playback.due());
        }

        if (_settling) {
            until(_movedAt + static_cast<std::uint64_t>(Refiner::REST_MS));
        }

        return wait;
    }

    // Along the bottom of the view, over the image.
    Rect App::play_bar() const {
        const auto height = static_cast<double>(PlayBar::height(SDL_GetWindowDisplayScale(_window)));

        return {0.0, _viewport.area_height() - height, _viewport.area_width(), height};
    }

    std::string App::bar_left() const {
        return std::format("[{}/{}] {}", _folder.index() + 1, _folder.count(), _folder.current().filename().string());
    }

    std::string App::bar_right() const {
        if (_info.width == 0) {
            return human_size(_bytes);
        }

        if (_playback.active()) {
            return std::format("{}x{}, {} {}/{}, {}", _info.width, _info.height, _info.format, _playback.frame() + 1, _playback.frames(), human_size(_bytes));
        }

        return std::format("{}x{}, {}, {}", _info.width, _info.height, _info.format, human_size(_bytes));
    }
}
