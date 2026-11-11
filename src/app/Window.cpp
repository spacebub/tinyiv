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
#include <cstdio>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "app/Config.h"
#include "app/Window.h"
#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "render/Canvas.h"
#include "render/Tiles.h"
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

// The device the gpu renderer would make, kept so a renderer for another output reuses it. It turns off
// the same features:
// https://github.com/libsdl-org/SDL/blob/release-3.4.x/src/render/gpu/SDL_render_gpu.c
// Vulkan comes first, as SDL's Direct3D 12 backend never presents HDR, then whatever SDL picks.
SDL_GPUDevice *create_device() {
    const SDL_PropertiesID properties = SDL_CreateProperties();

    // Debug mode asserts when the window grows before the backbuffer does, as on going fullscreen,
    // and the assertion prompt blocks the loop.
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, false);

    SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");

    SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(properties);

    if (device == nullptr) {
        SDL_ClearProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING);
        device = SDL_CreateGPUDeviceWithProperties(properties);
    }

    SDL_DestroyProperties(properties);

    return device;
}

SDL_Renderer *create_renderer(SDL_Window *window, SDL_GPUDevice *device, const bool linear) {
    const SDL_PropertiesID properties = SDL_CreateProperties();

    SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetStringProperty(properties, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
    SDL_SetPointerProperty(properties, SDL_PROP_RENDERER_CREATE_GPU_DEVICE_POINTER, device);
    SDL_SetNumberProperty(properties,
                          SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER,
                          linear ? SDL_COLORSPACE_SRGB_LINEAR : SDL_COLORSPACE_SRGB);

    SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);

    SDL_DestroyProperties(properties);

    return renderer;
}

// SDL gives SDR white in scRGB units, where 1.0 is 80 nits.
float sdr_white_nits(SDL_Window *window) {
    constexpr float SCRGB_UNIT_NITS = 80.0F;

    return SDL_GetFloatProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0F)
           * SCRGB_UNIT_NITS;
}

// SDL gives the peak as headroom above SDR white.
float peak_nits(SDL_Window *window) {
    return SDL_GetFloatProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0F)
           * sdr_white_nits(window);
}

// An hdr_white at the peak or past it would leave no headroom, and HDR images would show as SDR.
float shown_white(SDL_Window *window, const float wanted) {
    return std::min(wanted, peak_nits(window) / MIN_HDR_HEADROOM);
}

// Zero is mailbox on the gpu renderer, see ChoosePresentMode in SDL_render_gpu.c, so each refresh
// shows the newest frame. Under FIFO a drag that starts from idle queues frames ahead of the display
// and stutters until it fills.
int vsync_setting(SDL_Renderer *renderer, SDL_Window *window) {
    auto *device = static_cast<SDL_GPUDevice *>(
            SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));

    return device != nullptr && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX) ? 0 : 1;
}
}

Window::~Window() {
    _canvas = nullptr;
    _sdl = nullptr;
    _vulkan = nullptr;

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

    const char *renderer = SDL_getenv("TIV_RENDERER");
    const bool own = renderer != nullptr && std::string_view(renderer) == "own";

    _window = SDL_CreateWindow("tinyiv",
                               scaled(usable.w, WINDOW_SHARE),
                               scaled(usable.h, WINDOW_SHARE),
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (own ? SDL_WINDOW_VULKAN : 0));

    if (_window == nullptr) {
        *error = SDL_GetError();

        return false;
    }

    SDL_SetWindowMinimumSize(_window, MIN_WIDTH, MIN_HEIGHT);

    // Windows takes the icon from the executable's resources.
#ifndef _WIN32
    set_icon(_window);
#endif

    if (own) {
        return make_vulkan(error);
    }

    // Kept to share between renderers, as an HDR display makes a second one.
    _device = create_device();

    if (!make_renderer(false)) {
        *error = SDL_GetError();

        return false;
    }

    return true;
}

bool Window::make_vulkan(std::string *error) {
    _vulkan = gpu::VulkanPainter::create(_window, error);

    if (_vulkan == nullptr) {
        return false;
    }

    _canvas = std::make_unique<Canvas>(*_vulkan, Canvas::DEFAULT_VRAM_BUDGET, _lean);

    return true;
}

// The present mode can change whenever the swapchain is made again.
bool Window::vsync() const {
    return _vulkan != nullptr ? _vulkan->waits_for_display() : _vsync;
}

Painter *Window::painter() const {
    if (_vulkan != nullptr) {
        return _vulkan->lost() ? nullptr : _vulkan.get();
    }

    return _sdl.get();
}

// Linear output is what carries HDR. The window has one renderer, so another output means
// another renderer, and the canvas with its textures goes with the old one.
bool Window::make_renderer(const bool linear) {
    _canvas = nullptr;
    _sdl = nullptr;

    if (_renderer != nullptr) {
        SDL_DestroyRenderer(_renderer);
    }

    _renderer = _device != nullptr ? create_renderer(_window, _device, linear) : nullptr;

    // Else whichever renderer SDL has.
    if (_renderer == nullptr && !linear) {
        _renderer = SDL_CreateRenderer(_window, nullptr);
    }

    if (_renderer == nullptr) {
        return false;
    }

    _hdr10 = linear;

    const int vsync = vsync_setting(_renderer, _window);

    SDL_SetRenderVSync(_renderer, vsync);
    _vsync = vsync != 0;

    _sdl = std::make_unique<SdlPainter>(_renderer);
    _canvas = std::make_unique<Canvas>(*_sdl, Canvas::DEFAULT_VRAM_BUDGET, _lean);

    return true;
}

// SDR images stay on the SDR output, only PQ ones switch to HDR: HDR10 on Vulkan, linear on SDL.
void Window::follow_output(const Loader::Result &result) {
    bool pq = false;

    if (result.tileCache != nullptr) {
        pq = result.tileCache->encoding() == Bitmap::Encoding::Pq;
    } else if (result.pyramid != nullptr && !result.pyramid->empty()) {
        pq = result.pyramid->levels.front()->encoding() == Bitmap::Encoding::Pq;
    }

    const bool hdr = pq && _display.hdr();

    if (_vulkan != nullptr) {
        // The device and every tile stay, only the swapchain is made again.
        if (hdr != _hdr10) {
            _vulkan->set_output(hdr ? gpu::Swapchain::Output::Hdr10 : gpu::Swapchain::Output::Sdr);
            _hdr10 = _vulkan->output() == gpu::Swapchain::Output::Hdr10;
        }

        return;
    }

    if (hdr == _hdr10) {
        return;
    }

    if (!make_renderer(hdr) && hdr && !make_renderer(false)) {
        std::println(stderr, "tinyiv: {}", SDL_GetError());
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

// HDR images decode for the headroom the window has above hdr_white, and show again once decoded.
// SDL measures that headroom from the SDR white of the display instead.
bool Window::update_display() {
    if (_vulkan != nullptr) {
        return update_vulkan_display();
    }

    const SDL_PropertiesID properties = SDL_GetWindowProperties(_window);
    const bool hdr =
            _device != nullptr && SDL_GetBooleanProperty(properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    // Rounded as cache names round it, so a change in the last bit of SDR white reloads nothing.
    const float headroom =
            hdr ? std::round(peak_nits(_window) / shown_white(_window, _hdrWhite) * HEADROOM_STEPS) / HEADROOM_STEPS
                : 1.0F;

    if (headroom == _display.headroom) {
        return false;
    }

    _display.headroom = headroom;

    return true;
}

// The Vulkan renderer passes PQ through, so white goes into the encoding of the images. On Wayland the
// compositor anchors PQ reference white, 203 nits, to the reference of the display. White at W nits is
// then encoded at W x 203 / reference.
bool Window::update_vulkan_display() {
    const WaylandColor::Output *wayland = _vulkan->wayland_output();
    const bool hdr = _vulkan->supports(gpu::Swapchain::Output::Hdr10)
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

    _vulkan->set_hdr({
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

// Linear output puts white at the SDR white of the display, which hdr_white overrides.
float Window::color_scale() const {
    return _hdr10 && _renderer != nullptr ? shown_white(_window, _hdrWhite) / sdr_white_nits(_window) : 1.0F;
}
}
