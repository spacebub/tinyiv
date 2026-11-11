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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <SDL3/SDL.h>

#include "app/Config.h"
#include "app/Window.h"
#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "render/Canvas.h"
#include "render/WaylandColor.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/VulkanPainter.h"
#include "services/Loader.h"

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
// Room for the top bar to show a file name beside the order.
constexpr int MIN_WIDTH = 480;
constexpr int MIN_HEIGHT = 270;
// Keeps white at or below the knee of the roll off, which starts at 80% of the headroom.
constexpr float MIN_HDR_HEADROOM = 1.25F;
constexpr float HEADROOM_STEPS = 100.0F;
// Where the display gives no refresh rate.
constexpr float FALLBACK_REFRESH_RATE = 60.0F;
// SDL counts light in scRGB units, where 1.0 is 80 nits.
constexpr float SCRGB_UNIT_NITS = 80.0F;

#ifndef _WIN32
SDL_Surface *icon_surface(Bitmap &icon) {
    return SDL_CreateSurfaceFrom(
            icon.width(), icon.height(), SDL_PIXELFORMAT_RGBA32, icon.data(), static_cast<int>(icon.pitch()));
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

int scaled(const int value, const float by) {
    return static_cast<int>(static_cast<float>(value) * by);
}

// Used off Wayland only. There SDL reports the white level of the display.
float sdr_white_nits(SDL_Window *window) {
    return SDL_GetFloatProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0F)
           * SCRGB_UNIT_NITS;
}

// SDL gives the peak as headroom above SDR white.
float peak_nits(SDL_Window *window) {
    return SDL_GetFloatProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0F)
           * sdr_white_nits(window);
}
}

Window::Window() = default;

Window::~Window() {
    _canvas = nullptr;
    _painter = nullptr;

    if (_window != nullptr) {
        SDL_DestroyWindow(_window);
    }

    SDL_Quit();
}

bool Window::open(const Config &config, std::string *error) {
    _awake = config.awake;
    _hdrWhite = static_cast<float>(config.hdrWhite);
    _lean = config.footprint;

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

    follow_awake();

    SDL_Rect usable{.x = 0, .y = 0, .w = 1280, .h = 800};

    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);

    _window = SDL_CreateWindow("tinyiv",
                               scaled(usable.w, WINDOW_SHARE),
                               scaled(usable.h, WINDOW_SHARE),
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN
                                       | SDL_WINDOW_HIDDEN);

    if (_window == nullptr) {
        *error = SDL_GetError();

        return false;
    }

    SDL_SetWindowMinimumSize(_window, MIN_WIDTH, MIN_HEIGHT);

    // Windows takes the icon from the executable's resources.
#ifndef _WIN32
    set_icon(_window);
#endif

    _painter = gpu::VulkanPainter::create(_window, error);

    if (_painter == nullptr) {
        return false;
    }

    _canvas = std::make_unique<Canvas>(*_painter, Canvas::DEFAULT_VRAM_BUDGET, _lean);
    // Shown only now, so a machine without Vulkan sees no window come and go.
    SDL_ShowWindow(_window);

    return true;
}

// The present mode can change whenever the swapchain is made again.
bool Window::vsync() const {
    return _painter->waits_for_display();
}

bool Window::hdr10() const {
    return _painter->output() == gpu::Swapchain::Output::Hdr10;
}

Painter *Window::painter() const {
    return _painter->lost() ? nullptr : _painter.get();
}

// SDR images stay on the SDR output, only PQ ones switch to HDR10.
void Window::follow_output(const Loader::Result &result) {
    bool pq = false;

    if (result.tileCache != nullptr) {
        pq = result.tileCache->encoding() == Bitmap::Encoding::Pq;
    } else if (result.pyramid != nullptr && !result.pyramid->empty()) {
        pq = result.pyramid->levels.front()->encoding() == Bitmap::Encoding::Pq;
    }

    const bool hdr10 = pq && _display.hdr();

    // Only the swapchain is made again. The device and the tiles stay.
    if (hdr10 != this->hdr10()) {
        _painter->set_output(hdr10 ? gpu::Swapchain::Output::Hdr10 : gpu::Swapchain::Output::Sdr);
    }
}

void Window::set_fullscreen(const bool on) {
    _fullscreen = on;

    SDL_SetWindowFullscreen(_window, on);
    follow_awake();
}

void Window::follow_awake() const {
    if (_awake == Config::KeepAwake::Always || (_awake == Config::KeepAwake::Fullscreen && _fullscreen)) {
        SDL_DisableScreenSaver();
    } else {
        SDL_EnableScreenSaver();
    }
}

// The renderer passes PQ through, so white goes into the encoding of the images. A Wayland compositor
// anchors PQ reference white, 203 nits, to the reference of the display. White at W nits is then
// encoded at W x 203 / reference.
bool Window::update_display() {
    const WaylandColor::Output *wayland = _painter->wayland_output();
    const bool hdr = _painter->supports(gpu::Swapchain::Output::Hdr10)
                     && (wayland != nullptr ? wayland->hdr
                                            : SDL_GetBooleanProperty(SDL_GetWindowProperties(_window),
                                                                     SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,
                                                                     false));
    const float peak = wayland != nullptr ? wayland->peak : peak_nits(_window);
    const float reference = wayland != nullptr ? wayland->reference : Tone::PQ_REFERENCE_NITS;
    Tone::Display display;

    if (hdr && peak > 0.0F && reference > 0.0F) {
        const float white = std::min(_hdrWhite, peak / MIN_HDR_HEADROOM);

        display.headroom = std::round(peak / white * HEADROOM_STEPS) / HEADROOM_STEPS;
        display.whiteNits = std::round(white * Tone::PQ_REFERENCE_NITS / reference);
    }

    // The description counts in the encoding, where the reference of the display is 203.
    const float scale = reference > 0.0F ? Tone::PQ_REFERENCE_NITS / reference : 1.0F;

    _painter->set_hdr({
            .minimum = (wayland != nullptr ? wayland->minimum : 0.0F) * scale,
            .peak = peak * scale,
            .whiteNits = display.whiteNits,
    });

    if (display == _display) {
        return false;
    }

    _display = display;

    return true;
}

void Window::update_refresh() {
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(_window));

    _refreshRate = mode != nullptr && mode->refresh_rate > 0.0F ? mode->refresh_rate : FALLBACK_REFRESH_RATE;
}

Window::Size Window::pixels() const {
    Size size;

    SDL_GetWindowSizeInPixels(_window, &size.width, &size.height);

    return size;
}

std::optional<Window::Size> Window::desktop_pixels() const {
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(_window));

    if (desktop == nullptr) {
        return std::nullopt;
    }

    return Size{
            .width = scaled(desktop->w, desktop->pixel_density),
            .height = scaled(desktop->h, desktop->pixel_density),
    };
}

float Window::scale() const {
    return SDL_GetWindowDisplayScale(_window);
}

float Window::density() const {
    return SDL_GetWindowPixelDensity(_window);
}
}
