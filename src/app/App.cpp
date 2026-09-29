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
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/App.h"
#include "app/Config.h"
#include "app/Input.h"
#include "gallery/Folder.h"
#include "image/Bitmap.h"
#include "image/Orient.h"
#include "image/Reorient.h"
#include "image/decode/Decode.h"
#include "render/Canvas.h"
#include "render/PlayBar.h"
#include "render/StatusBar.h"
#include "services/Loader.h"
#include "services/Memory.h"
#include "services/Refiner.h"
#include "tiv_git_revision.h"
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
        // How long fullscreen shows the streaming mode or the order after it switches.
        constexpr std::uint64_t MODE_FLASH_MS = 1500;
        // A scalable image renders this share of the view beyond each edge, so a short pan stays sharp.
        constexpr double REFINE_MARGIN = 0.25;
        constexpr SDL_Color BACKGROUND{.r = 0, .g = 0, .b = 0, .a = 255};

        // Keys are two spaces apart, the words of one key a single space.
        constexpr std::array<StatusBar::Row, 18> HELP = {
                {
                        {.left = "Wheel  ←  →", .right = "Previous, next image"},
                        {.left = "Home  End", .right = "First, last image"},
                        {.left = "O", .right = "Sort by name, date, size"},
                        {.left = "R", .right = "Reload from disk"},
                        {.left = "Left drag", .right = "Pan"},
                        {.left = "Right drag ↑ ↓", .right = "Zoom in, out"},
                        {.left = "↑  ↓", .right = "Zoom in, out a step"},
                        {.left = "0  1  2", .right = "Centre, fit, actual size"},
                        {.left = "F  F11  Double click", .right = "Fullscreen"},
                        {.left = "[  ]", .right = "Turn left, right"},
                        {.left = ";  '", .right = "Flip vertically, horizontally"},
                        {.left = "Ctrl+S", .right = "Save turns and flips"},
                        {.left = "S", .right = "Streaming mode on, off"},
                        {.left = "I", .right = "Status bar on, off"},
                        {.left = "Space", .right = "Play, pause animation"},
                        {.left = ",  .", .right = "Previous, next frame"},
                        {.left = "Ctrl+H", .right = "Show, hide this help"},
                        {.left = "Esc  Q  Ctrl+D", .right = "Quit"},
                },
        };

#ifndef _WIN32
        SDL_Surface *icon_surface(Bitmap &icon) {
            return SDL_CreateSurfaceFrom(icon.width(), icon.height(), SDL_PIXELFORMAT_RGBA32, icon.data(),
                                         static_cast<int>(icon.pitch()));
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

        // In the order O steps through them.
        struct OrderText {
            Folder::Order order;
            // Leads the status bar for as long as the order holds.
            std::string_view tag;
            // Said for a moment when O switches to it.
            std::string_view text;
        };

        constexpr std::array ORDERS{
                OrderText{.order = Folder::Order::AToZ, .tag = "A-Z", .text = "Sorted by name, A to Z"},
                OrderText{.order = Folder::Order::ZToA, .tag = "Z-A", .text = "Sorted by name, Z to A"},
                OrderText{.order = Folder::Order::Newest, .tag = "NEWEST", .text = "Sorted by date, newest first"},
                OrderText{.order = Folder::Order::Oldest, .tag = "OLDEST", .text = "Sorted by date, oldest first"},
                OrderText{
                        .order = Folder::Order::Smallest, .tag = "SMALLEST", .text = "Sorted by size, smallest first"},
        };

        const OrderText &order_text(const Folder::Order order) {
            return *std::ranges::find(ORDERS, order, &OrderText::order);
        }

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

        // What the gpu renderer would make for itself, kept so a renderer made again for another
        // output skips making a device. The features are the ones it turns off:
        // https://github.com/libsdl-org/SDL/blob/release-3.4.x/src/render/gpu/SDL_render_gpu.c
        SDL_GPUDevice *create_device() {
            const SDL_PropertiesID properties = SDL_CreateProperties();

            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN,
                                   true);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN,
                                   false);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
            SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, false);

            SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(properties);

            SDL_DestroyProperties(properties);

            return device;
        }

        SDL_Renderer *create_renderer(SDL_Window *window, SDL_GPUDevice *device, const bool linear) {
            const SDL_PropertiesID properties = SDL_CreateProperties();

            SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
            SDL_SetStringProperty(properties, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
            SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_DEVICE_POINTER, device);
            SDL_SetNumberProperty(properties, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER,
                                  linear ? SDL_COLORSPACE_SRGB_LINEAR : SDL_COLORSPACE_SRGB);

            SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);

            SDL_DestroyProperties(properties);

            return renderer;
        }

        // Zero is mailbox on the gpu renderer, see ChoosePresentMode in SDL_render_gpu.c, so each refresh
        // shows the newest frame. Under FIFO a drag that starts from idle queues frames ahead of the display
        // and stutters until it fills.
        int vsync_setting(SDL_Renderer *renderer, SDL_Window *window) {
            auto *device = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(
                    SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));

            return device != nullptr && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)
                           ? 0
                           : 1;
        }
    }

    App::~App() {
        _refiner = nullptr;
        _loader = nullptr;
        _canvas = nullptr;

        if (_renderer != nullptr) {
            SDL_DestroyRenderer(_renderer);
        }

        if (_device != nullptr) {
            SDL_DestroyGPUDevice(_device);
        }

        if (_window != nullptr) {
            SDL_DestroyWindow(_window);
        }

        SDL_Quit();
    }

    bool App::start(const std::filesystem::path &file, std::string *error) {
        std::vector<std::string> warnings;
        const Config config = Config::load(Config::location(), &warnings);

        for (const std::string &warning : warnings) {
            std::println(stderr, "tinyiv: {}", warning);
        }

        _folder.sort(config.order);

        if (!file.empty() && !_folder.open(file, Decode::suffixes(), error)) {
            return false;
        }

        // The first decode starts before the window exists, so the two overlap.
        _loaderEvent = SDL_RegisterEvents(3);
        _refinerEvent = _loaderEvent + 1;
        _tileCacheEvent = _loaderEvent + 2;
        _loader = std::make_unique<Loader>(_loaderEvent);

        _loader->set_tiles(config.cache, config.small, config.persist);
        _loader->stream_all(config.streaming);
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
        update_display();
        // Said even when SDR, since HDR images wait for it before they build pyramids of tiles.
        _loader->set_display(_display);
        // A result posted before SDL_Init has no event queue to wake the loop.
        deliver();

        return true;
    }

    bool App::open_window(std::string *error) {
        // The identifier is the window's app id, which is how the desktop finds the icon.
        SDL_SetAppMetadata("tinyiv", TIV_VERSION, "tinyiv");
        // Drags at the window edge run in relative mode, where motion is otherwise raw and a pan would lag the pointer.
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_CURSOR_VISIBLE, "1");
        // Centring would pull the visible cursor to the middle of the window where the system confines it.
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "0");

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            *error = SDL_GetError();

            return false;
        }

        SDL_Rect usable{.x = 0, .y = 0, .w = 1280, .h = 800};

        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);

        _window = SDL_CreateWindow("tinyiv", scaled(usable.w, WINDOW_SHARE), scaled(usable.h, WINDOW_SHARE),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

        if (_window == nullptr) {
            *error = SDL_GetError();

            return false;
        }

        SDL_SetWindowMinimumSize(_window, MIN_WIDTH, MIN_HEIGHT);

        // Windows takes the icon from the executable's resources.
#ifndef _WIN32
        set_icon(_window);
#endif

        // The device is kept to share between renderers, and only an HDR display ever makes a second one.
        if (SDL_GetBooleanProperty(SDL_GetWindowProperties(_window), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) {
            _device = create_device();
        }

        if (!make_renderer(false)) {
            *error = SDL_GetError();

            return false;
        }

        return true;
    }

    // Linear output is what carries HDR. The window has one renderer, so another output means
    // another renderer, and the canvas with its textures goes with the old one.
    bool App::make_renderer(const bool linear) {
        _canvas = nullptr;

        if (_renderer != nullptr) {
            SDL_DestroyRenderer(_renderer);
        }

        _renderer = _device != nullptr ? create_renderer(_window, _device, linear) : nullptr;

        if (_renderer == nullptr && !linear) {
            _renderer = SDL_CreateRenderer(_window, "gpu");

            if (_renderer == nullptr) {
                _renderer = SDL_CreateRenderer(_window, nullptr);
            }
        }

        if (_renderer == nullptr) {
            return false;
        }

        _linear = linear;
        SDL_SetRenderVSync(_renderer, vsync_setting(_renderer, _window));

        const auto maxTexture = static_cast<int>(SDL_GetNumberProperty(
                SDL_GetRendererProperties(_renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, Tiles::SIZE));
        _canvas = std::make_unique<Canvas>(_renderer, maxTexture);
        _dirty = true;

        return true;
    }

    // SDR images stay on the SDR output, only PQ ones switch to linear.
    void App::follow_output(const Loader::Result &result) {
        bool pq = false;

        if (result.tileCache != nullptr) {
            pq = result.tileCache->encoding() == Bitmap::Encoding::Pq;
        } else if (result.pyramid != nullptr && !result.pyramid->empty()) {
            pq = result.pyramid->levels.front()->encoding() == Bitmap::Encoding::Pq;
        }

        const bool linear = pq && _display.hdr();

        if (linear == _linear) {
            return;
        }

        if (!make_renderer(linear) && linear && !make_renderer(false)) {
            std::println(stderr, "tinyiv: {}", SDL_GetError());
            _running = false;
        }
    }

    void App::layout() {
        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(_window, &width, &height);

        const int bar = bar_shown() ? StatusBar::height(SDL_GetWindowDisplayScale(_window)) : 0;

        _viewport.set_area(width, height - bar);
        _repaints = REPAINTS;
        moved();
    }

    void App::set_fullscreen(const bool on) {
        _fullscreen = on;

        SDL_SetWindowFullscreen(_window, on);
        layout();
    }

    bool App::bar_shown() const {
        return _fullscreen ? _barFullscreen : _barWindowed;
    }

    void App::toggle_bar() {
        bool &shown = _fullscreen ? _barFullscreen : _barWindowed;

        shown = !shown;
        layout();
        _dirty = true;
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
        _building = false;
        _streamed = false;
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
        _shownAt = SDL_GetTicks();
        _dirty = true;
    }

    void App::run() {
        while (_running) {
            // Asks for the streamed tiles the view meets before deciding there is nothing to do.
            _canvas->fetch(_viewport);

            const bool idle = !_canvas->pending(_viewport) && _repaints == 0;

            // Nothing to upload or repaint means nothing to do until something happens, except
            // that a loading indicator has to keep moving and an animation has frames due.
            if (idle) {
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
                // Presented from idle, a frame can land before the compositor has caught up and
                // stay unseen until the next, so every change is drawn once more after it.
                _repaints = _dirty ? std::max(_repaints - 1, 1) : std::max(_repaints - 1, 0);
                _dirty = false;
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

        // The loop uploads it next.
        if (event.type == _tileCacheEvent) {
            _dirty = true;

            return;
        }

        if (is_window_event(event)) {
            layout();
            update_display();

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
            case Input::Action::ToggleStream:
                toggle_stream();
                break;
            case Input::Action::NextOrder:
                next_order();
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
        const Rect box = StatusBar::table_box(
                {.x = 0.0, .y = 0.0, .width = static_cast<double>(width), .height = static_cast<double>(height)},
                SDL_GetWindowDisplayScale(_window), HELP);
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
                _failure = result.unsupported ? "Unsupported file format" : failure_text(result.error);
                _loading = false;
                _shown = result.generation;
                _viewport.set_image(0, 0);
                _dirty = true;

                continue;
            }

            // Nothing to draw until the tiles are made, so the old image goes and the view
            // says how far they are.
            if (result.kind == Loader::Kind::Building) {
                _shown = result.generation;
                _info = result.info;
                _turn = 1;
                _building = true;
                _tileFolder = result.tileFolder;
                _loading = true;
                _canvas->clear();
                _viewport.set_image(0, 0);
                _dirty = true;

                continue;
            }

            // The first result of a new image replaces the old one whole.
            if (_shown != result.generation || _building) {
                _shown = result.generation;
                _info = result.info;
                _turn = 1;
                _building = false;

                const Size size = shown();

                _viewport.set_image(size.width, size.height);
                moved();
            }

            if (result.animation != nullptr && result.animation == _playback.animation()) {
                continue;
            }

            const Size size = stored();
            const auto image = static_cast<std::uint64_t>(_folder.index());

            _streamed = result.tileCache != nullptr;
            follow_output(result);

            if (_streamed) {
                // Each tile read wakes the loop, which uploads it.
                result.tileCache->on_ready([event = _tileCacheEvent] {
                    SDL_Event ready{};

                    ready.type = event;
                    SDL_PushEvent(&ready);
                });

                _canvas->show(result.tileCache, image, size.width, size.height, orientation());
            } else {
                _canvas->show(result.pyramid, image, size.width, size.height, orientation());
            }

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
                case Loader::Kind::Building:
                case Loader::Kind::Failed:
                    break;
            }

            _dirty = true;
        }

        warm_neighbours();
        _giveBack = true;
    }

    // Once everything has arrived and navigation rests, what the old images held goes back to the system.
    void App::give_back() {
        if (_giveBack && !_loading && SDL_GetTicks() - _shownAt >= static_cast<std::uint64_t>(Loader::GIVE_BACK_MS)
            && _loader->idle()) {
            Memory::give_back();
            _giveBack = false;
        }
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

            return oriented({.x = cx0, .y = cy0, .width = cx1 - cx0, .height = cy1 - cy0}, back);
        };

        const Rect seen = unit(left, top, right, bottom);
        const Rect seenPixels{
                .x = seen.x * size.width,
                .y = seen.y * size.height,
                .width = seen.width * size.width,
                .height = seen.height * size.height,
        };

        if (_canvas->refined(zoom, seenPixels)) {
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

                _canvas->warm(std::move(pyramid), static_cast<std::uint64_t>(index), swapped ? info.height : info.width,
                              swapped ? info.width : info.height, info.orientation);
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

    // HDR images decode for the headroom the window has now, and show again once they have.
    void App::update_display() {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(_window);
        const bool hdr =
                _device != nullptr && SDL_GetBooleanProperty(properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
        const float headroom =
                hdr ? std::max(SDL_GetFloatProperty(properties, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0F), 1.0F) : 1.0F;

        if (headroom == _display.headroom) {
            return;
        }

        _display.headroom = headroom;
        _loader->set_display(_display);

        if (_info.hdr && _folder.count() > 0) {
            _canvas->clear();
            show(_folder.index(), _direction);
        }
    }

    void App::toggle_stream() {
        _switchedAt = SDL_GetTicks();
        _dirty = true;

        // It streams whatever the mode, so switching would only decode it again the same way.
        if (always_streamed()) {
            _flash = "Too large for memory, always streamed";

            return;
        }

        const bool on = !_loader->streaming_all();

        _flash = on ? "Streaming mode on" : "Streaming mode off";
        _loader->stream_all(on);
        // Tiles of the same image and size would be kept, so the canvas lets go of them first.
        _canvas->clear();

        if (_folder.count() == 0) {
            return;
        }

        show(_folder.index(), _direction);
    }

    // The images prefetched stay as they are, and the next step fetches the new neighbours.
    void App::next_order() {
        const OrderText *at = &order_text(_folder.order());
        const OrderText &next = std::next(at) == ORDERS.end() ? ORDERS.front() : *std::next(at);

        _folder.sort(next.order);
        _flash = next.text;
        _switchedAt = SDL_GetTicks();
        _dirty = true;
    }

    std::string App::where_tiles_go() const {
        if (_tileFolder.empty()) {
            return {};
        }

#ifdef _WIN32
        const char *home = SDL_getenv("USERPROFILE");
#else
        const char *home = SDL_getenv("HOME");
#endif
        const std::string folder = _tileFolder.string();

        if (home != nullptr && *home != '\0' && folder.starts_with(home)) {
            return " in ~" + folder.substr(std::string_view(home).size());
        }

        return " in " + folder;
    }

    std::string App::mode_text() const {
        return _flash;
    }

    bool App::always_streamed() const {
        return (_streamed || _building) && static_cast<std::int64_t>(_info.width) * _info.height > Loader::max_pixels();
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
        return Orient::swaps(_info.orientation) ? Size{.width = _info.height, .height = _info.width}
                                                : Size{.width = _info.width, .height = _info.height};
    }

    App::Size App::shown() const {
        return Orient::swaps(_turn) ? Size{.width = _info.height, .height = _info.width}
                                    : Size{.width = _info.width, .height = _info.height};
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
            StatusBar::notice(_renderer,
                              {.x = 0.0, .y = 0.0, .width = _viewport.area_width(), .height = _viewport.area_height()},
                              scale, _failure);
        } else if (_building) {
            const std::string notice =
                    std::format("Making tiles{}: {:.0f}%", where_tiles_go(), _loader->progress() * 100.0F);

            StatusBar::notice(_renderer,
                              {.x = 0.0, .y = 0.0, .width = _viewport.area_width(), .height = _viewport.area_height()},
                              scale, notice);
        } else if (_folder.count() == 0) {
            StatusBar::notice(_renderer,
                              {.x = 0.0, .y = 0.0, .width = _viewport.area_width(), .height = _viewport.area_height()},
                              scale, "Drop an image here to open it");
        }

        if (_playback.active()) {
            PlayBar::draw(_renderer, play_bar(), scale, _playback.playing(), _playback.progress());
        }

        if (_help) {
            StatusBar::table(
                    _renderer,
                    {.x = 0.0, .y = 0.0, .width = static_cast<double>(width), .height = static_cast<double>(height)},
                    scale, HELP);
        }

        if (bar_shown()) {
            const int bar = StatusBar::height(scale);

            StatusBar::draw(_renderer,
                            {
                                    .x = 0.0,
                                    .y = static_cast<double>(height - bar),
                                    .width = static_cast<double>(width),
                                    .height = static_cast<double>(bar),
                            },
                            scale, bar_left(), bar_right(), _loading);
        } else if (flashing()) {
            // No bar to carry the mode, so switching it says so for a moment.
            StatusBar::badge(_renderer, BADGE_MARGIN * scale, height - (BADGE_MARGIN * scale), scale, mode_text(),
                             false);
        } else if (_loading) {
            // Where the bar would be, for as long as something is still on its way.
            StatusBar::badge(_renderer, BADGE_MARGIN * scale, height - (BADGE_MARGIN * scale), scale, bar_left(false),
                             true);
        }

        SDL_RenderPresent(_renderer);
    }

    void App::wait_for_event() {
        give_back();

        SDL_Event event{};
        const int wait = wait_ms();

        if (wait < 0 ? SDL_WaitEvent(&event) : SDL_WaitEventTimeout(&event, wait)) {
            handle(event);
        } else {
            // Every timeout is a deadline that changes the frame, such as the badge going away.
            _dirty = true;
        }
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

        // Wakes to take the badge away again.
        if (flashing()) {
            until(_switchedAt + MODE_FLASH_MS);
        }

        // Past it, the next event or delivery gives memory back, and waking early would only spin.
        if (const std::uint64_t due = _shownAt + static_cast<std::uint64_t>(Loader::GIVE_BACK_MS);
            _giveBack && due > now) {
            until(due);
        }

        return wait;
    }

    // Along the bottom of the view, over the image.
    Rect App::play_bar() const {
        const auto height = static_cast<double>(PlayBar::height(SDL_GetWindowDisplayScale(_window)));

        return {.x = 0.0, .y = _viewport.area_height() - height, .width = _viewport.area_width(), .height = height};
    }

    bool App::flashing() const {
        return _switchedAt != 0 && SDL_GetTicks() - _switchedAt < MODE_FLASH_MS;
    }

    // Loader errors lead with the file's path, which the text drops to keep only the reason.
    std::string App::failure_text(const std::string &error) const {
        const std::string prefix = _folder.count() > 0 ? _folder.current().string() + ": " : std::string();

        if (prefix.empty() || !error.starts_with(prefix) || error.size() == prefix.size()) {
            return "Could not open this image";
        }

        return "Could not open this image: " + error.substr(prefix.size());
    }

    std::string App::bar_left(const bool tagged) const {
        const std::string tag = std::string(tagged && (_streamed || _loader->streaming_all()) ? "STREAMING  " : "")
                                + (tagged && _linear ? "HDR  " : "");

        if (_folder.count() == 0) {
            return std::format("{}tinyiv", tag);
        }

        // Marked while turned or flipped and not saved, and led by the modes that are on and the order.
        return std::format("{}{}[{}/{}] {}{}", tag, tagged ? std::format("{}  ", order_text(_folder.order()).tag) : "",
                           _folder.index() + 1, _folder.count(), _folder.current().filename().string(),
                           _turn != 1 ? " *" : "");
    }

    std::string App::bar_right() const {
        // Opposite the name, while there is nothing else to say.
        if (_folder.count() == 0) {
            if constexpr (*TIV_GIT_REVISION != '\0') {
                return "v" TIV_VERSION " (" TIV_GIT_REVISION ")";
            }

            return "v" TIV_VERSION;
        }

        const Size size = shown();
        const std::string format = _info.hdr ? _info.format + " HDR" : _info.format;

        // Switching the mode says so for a moment, as the badge does in fullscreen.
        if (flashing()) {
            return mode_text();
        }

        if (!_message.empty()) {
            return _message;
        }

        if (_info.width == 0) {
            return human_size(_bytes);
        }

        if (_playback.active()) {
            return std::format("{}x{}, {} {}/{}, {}", size.width, size.height, format, _playback.frame() + 1,
                               _playback.frames(), human_size(_bytes));
        }

        return std::format("{}x{}, {}, {}", size.width, size.height, format, human_size(_bytes));
    }
}
