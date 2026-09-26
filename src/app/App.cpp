// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
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
#include "render/StatusBar.h"
#include "services/Loader.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        constexpr float WINDOW_SHARE = 0.8F;
        constexpr int PREFETCH_AHEAD = 6;
        constexpr int PREFETCH_BEHIND = 2;
        constexpr double BADGE_MARGIN = 12.0;
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
        _loaderEvent = SDL_RegisterEvents(1);
        _loader = std::make_unique<Loader>(_loaderEvent);

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
            // that a loading indicator has to keep moving.
            if (idle && _loading) {
                if (SDL_WaitEventTimeout(&event, StatusBar::LOADING_TICK_MS)) {
                    handle(event);
                } else {
                    _dirty = true;
                }
            } else if (idle && SDL_WaitEvent(&event)) {
                handle(event);
            }

            while (_running && SDL_PollEvent(&event)) {
                handle(event);
            }

            if (!_running) {
                break;
            }

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

        if (is_window_event(event)) {
            layout();

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
            case Input::Action::Redraw:
                _dirty = true;
                break;
            case Input::Action::None:
                break;
        }
    }

    void App::deliver() {
        Loader::Result result;

        while (_loader->take(&result)) {
            if (result.generation != _generation) {
                continue;
            }

            if (result.kind == Loader::Kind::Failed) {
                std::println(stderr, "tinyiv: {}", result.error);

                if (++_failures < _folder.count()) {
                    show(_folder.index() + _direction, _direction);
                }

                continue;
            }

            // The first result of a new image replaces the old one whole.
            if (_shown != result.generation) {
                _shown = result.generation;
                _info = result.info;
                _failures = 0;
                _viewport.set_image(_info.width, _info.height);
            }

            _canvas->show(result.pyramid, static_cast<std::uint64_t>(_folder.index()), _info.width, _info.height);
            // Straight away, or the frame between the old image and this one is blank.
            _canvas->upload(_viewport, UPLOAD_BUDGET);

            switch (result.kind) {
                case Loader::Kind::Preview:
                    _loading = true;
                    break;
                case Loader::Kind::Full:
                    _loading = false;
                    break;
                case Loader::Kind::Failed:
                    break;
            }

            _dirty = true;
        }

        warm_neighbours();
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

    std::string App::bar_left() const {
        return std::format("[{}/{}] {}", _folder.index() + 1, _folder.count(), _folder.current().filename().string());
    }

    std::string App::bar_right() const {
        if (_info.width == 0) {
            return human_size(_bytes);
        }

        return std::format("{}x{}, {}, {}", _info.width, _info.height, _info.format, human_size(_bytes));
    }
}
