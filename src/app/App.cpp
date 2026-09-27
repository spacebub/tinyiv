// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/App.h"
#include "app/Input.h"
#include "gallery/Folder.h"
#include "image/Bitmap.h"
#include "image/Decode.h"
#include "image/Orient.h"
#include "image/Reorient.h"
#include "render/Canvas.h"
#include "render/PlayBar.h"
#include "render/StatusBar.h"
#include "services/Loader.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

#ifndef _WIN32
namespace Embedded {
    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): tiv_embed defines them as C arrays.
    extern const unsigned char icon128[];
    extern const std::size_t icon128Size;
    extern const unsigned char icon256[];
    extern const std::size_t icon256Size;
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
}
#endif

namespace tiv {
    namespace {
        constexpr float WINDOW_SHARE = 0.8F;
        // Room for the status bar to show a file name beside the size and format.
        constexpr int MIN_WIDTH = 480;
        constexpr int MIN_HEIGHT = 270;
        constexpr int PREFETCH_AHEAD = 6;
        constexpr int PREFETCH_BEHIND = 2;
        constexpr double BADGE_MARGIN = 12.0;
        // A scalable image renders this share of the view beyond each edge, so a short pan stays sharp.
        constexpr double REFINE_MARGIN = 0.25;
        constexpr SDL_Color BACKGROUND{0, 0, 0, 255};

        // Keys apart by two spaces, words within one by one.
        constexpr std::array<StatusBar::Row, 15> HELP = {{
                {"Wheel  ←  →", "Previous, next image"},
                {"Home  End", "First, last image"},
                {"R", "Reload from disk"},
                {"Left drag", "Pan"},
                {"Right drag ↑ ↓", "Zoom in, out"},
                {"↑  ↓", "Zoom in, out a step"},
                {"1  2", "Fit, actual size"},
                {"F  F11  Double click", "Fullscreen"},
                {"[  ]", "Turn left, right"},
                {";  '", "Flip vertically, horizontally"},
                {"Ctrl+S", "Save turns and flips"},
                {"Space", "Play, pause animation"},
                {",  .", "Previous, next frame"},
                {"Ctrl+H", "Show, hide this help"},
                {"Esc  Q  Ctrl+D", "Quit"},
        }};

#ifndef _WIN32
        SDL_Surface *icon_surface(Bitmap &icon) {
            return SDL_CreateSurfaceFrom(icon.width(), icon.height(), SDL_PIXELFORMAT_RGBA32, icon.data(), static_cast<int>(icon.pitch()));
        }

        // The larger image is what a display at twice the density shows.
        void set_icon(SDL_Window *window) {
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay): each size comes alongside its array.
            const std::span<const std::uint8_t> smallPng(Embedded::icon128, Embedded::icon128Size);
            const std::span<const std::uint8_t> largePng(Embedded::icon256, Embedded::icon256Size);
            // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            Bitmap small;
            Bitmap large;

            if (!Decode::load_png_memory(smallPng, &small) || !Decode::load_png_memory(largePng, &large)) {
                return;
            }

            SDL_Surface *surface = icon_surface(small);
            SDL_Surface *alternate = icon_surface(large);

            if (surface != nullptr && alternate != nullptr && SDL_AddSurfaceAlternateImage(surface, alternate)) {
                SDL_SetWindowIcon(window, surface);
            }

            SDL_DestroySurface(alternate);
            SDL_DestroySurface(surface);
        }
#endif

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
        if (!file.empty() && !_folder.open(file, Decode::suffixes(), error)) {
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

        SDL_SetWindowMinimumSize(_window, MIN_WIDTH, MIN_HEIGHT);

        // Windows takes the icon from the executable's resources.
#ifndef _WIN32
        set_icon(_window);
#endif

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
        if (_folder.count() == 0) {
            return;
        }

        _folder.step(_folder.wrap(index) - _folder.index());
        _direction = direction;

        std::error_code failure;

        _bytes = std::filesystem::file_size(_folder.current(), failure);
        _info = {};
        _turn = 1;
        _message.clear();
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

        if (handle_help(event) || handle_play_bar(event)) {
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

        const Input::Action action = _input.handle(event, SDL_GetWindowPixelDensity(_window), _viewport);

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
            case Input::Action::Reload:
                reload();
                break;
            case Input::Action::TurnLeft:
                turn(Orient::TURN_LEFT);
                break;
            case Input::Action::TurnRight:
                turn(Orient::TURN_RIGHT);
                break;
            case Input::Action::FlipVertical:
                turn(Orient::FLIP_VERTICAL);
                break;
            case Input::Action::FlipHorizontal:
                turn(Orient::FLIP_HORIZONTAL);
                break;
            case Input::Action::Save:
                save();
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
                moved();
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

        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(_window, &width, &height);

        const float density = SDL_GetWindowPixelDensity(_window);
        const Rect box = StatusBar::table_box({0.0, 0.0, static_cast<double>(width), static_cast<double>(height)}, SDL_GetWindowDisplayScale(_window), HELP);
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
                _turn = 1;

                const Size size = shown();

                _viewport.set_image(size.width, size.height);
                moved();
            }

            if (result.animation != nullptr && result.animation == _playback.animation()) {
                continue;
            }

            const Size size = stored();

            _canvas->show(result.pyramid, static_cast<std::uint64_t>(_folder.index()), size.width, size.height, orientation());

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

        const Rect image = _viewport.image_rect();
        const double left = std::max(image.x, 0.0);
        const double top = std::max(image.y, 0.0);
        const double right = std::min(image.x + image.width, _viewport.area_width());
        const double bottom = std::min(image.y + image.height, _viewport.area_height());

        if (right <= left || bottom <= top) {
            return;
        }

        const double zoom = _viewport.zoom();
        const Size size = stored();
        const int back = Orient::inverse(orientation());

        // A rect on screen as a share of the image as stored, within the image.
        const auto unit = [&](const double x0, const double y0, const double x1, const double y1) {
            const double cx0 = std::clamp((x0 - image.x) / image.width, 0.0, 1.0);
            const double cy0 = std::clamp((y0 - image.y) / image.height, 0.0, 1.0);
            const double cx1 = std::clamp((x1 - image.x) / image.width, 0.0, 1.0);
            const double cy1 = std::clamp((y1 - image.y) / image.height, 0.0, 1.0);

            return oriented({cx0, cy0, cx1 - cx0, cy1 - cy0}, back);
        };

        const Rect seen = unit(left, top, right, bottom);

        if (_canvas->refined(zoom, {seen.x * size.width, seen.y * size.height, seen.width * size.width, seen.height * size.height})) {
            return;
        }

        const double marginX = _viewport.area_width() * REFINE_MARGIN;
        const double marginY = _viewport.area_height() * REFINE_MARGIN;
        const Rect wide = unit(left - marginX, top - marginY, right + marginX, bottom + marginY);
        const double scaledWidth = size.width * zoom;
        const double scaledHeight = size.height * zoom;
        const auto x = static_cast<int>(std::floor(wide.x * scaledWidth));
        const auto y = static_cast<int>(std::floor(wide.y * scaledHeight));
        const Ask ask{
                _generation,
                zoom,
                x,
                y,
                static_cast<int>(std::ceil((wide.x + wide.width) * scaledWidth)) - x,
                static_cast<int>(std::ceil((wide.y + wide.height) * scaledHeight)) - y,
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
                const bool swapped = Orient::swaps(info.orientation);

                _canvas->warm(std::move(pyramid), static_cast<std::uint64_t>(index), swapped ? info.height : info.width, swapped ? info.width : info.height, info.orientation);
            }
        }

        _canvas->keep(keep);
    }

    // Tiles of the same image and size would be kept, so the canvas lets go of them first.
    void App::reload() {
        if (_folder.count() == 0) {
            return;
        }

        _loader->forget(_folder.current());
        _canvas->clear();
        show(_folder.index(), _direction);
    }

    void App::turn(const int by) {
        if (!_viewport.has_image()) {
            return;
        }

        _turn = Orient::compose(by, _turn);
        _message.clear();
        _canvas->orient(orientation());

        const Size size = shown();

        _viewport.set_image(size.width, size.height);
        moved();
        _dirty = true;
    }

    void App::save() {
        if (_turn == 1 || _shown != _generation || _info.width == 0) {
            return;
        }

        _dirty = true;

        if (!Reorient::supported(_info.kind)) {
            _message = std::format("{} files keep no orientation, not saved", _info.format);

            return;
        }

        const int target = orientation();
        std::string error;

        if (!Reorient::write(_folder.current(), target, &error)) {
            std::println(stderr, "tinyiv: {}", error);
            _message = "Could not save";

            return;
        }

        _loader->reoriented(_folder.current(), target);

        if (Orient::swaps(_turn)) {
            std::swap(_info.width, _info.height);
        }

        std::error_code failure;

        _info.orientation = target;
        _turn = 1;
        _bytes = std::filesystem::file_size(_folder.current(), failure);
        _message = "Saved";
    }

    int App::orientation() const {
        return Orient::compose(_turn, _info.orientation);
    }

    App::Size App::stored() const {
        return Orient::swaps(_info.orientation) ? Size{_info.height, _info.width} : Size{_info.width, _info.height};
    }

    App::Size App::shown() const {
        return Orient::swaps(_turn) ? Size{_info.height, _info.width} : Size{_info.width, _info.height};
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
        } else if (_folder.count() == 0) {
            StatusBar::notice(_renderer, {0.0, 0.0, _viewport.area_width(), _viewport.area_height()}, scale, "Drop an image here to open it");
        }

        if (_playback.active()) {
            PlayBar::draw(_renderer, play_bar(), scale, _playback.playing(), _playback.progress());
        }

        if (_help) {
            StatusBar::table(_renderer, {0.0, 0.0, static_cast<double>(width), static_cast<double>(height)}, scale, HELP);
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
        if (_folder.count() == 0) {
            return "tinyiv";
        }

        // Marked while turned or flipped and not saved.
        return std::format("[{}/{}] {}{}", _folder.index() + 1, _folder.count(), _folder.current().filename().string(), _turn != 1 ? " *" : "");
    }

    std::string App::bar_right() const {
        if (_folder.count() == 0) {
            return {};
        }

        if (!_message.empty()) {
            return _message;
        }

        if (_info.width == 0) {
            return human_size(_bytes);
        }

        const Size size = shown();

        if (_playback.active()) {
            return std::format("{}x{}, {} {}/{}, {}", size.width, size.height, _info.format, _playback.frame() + 1, _playback.frames(), human_size(_bytes));
        }

        return std::format("{}x{}, {}, {}", size.width, size.height, _info.format, human_size(_bytes));
    }
}
