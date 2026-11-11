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
#include <memory>
#include <string_view>

#include <SDL3/SDL.h>

#include "render/WaylandColor.h"

#ifdef TIV_WAYLAND
#include <unistd.h>

#include <wayland-client.h>

#include "color-management-v1.h"
#include "image/Tone.h"

namespace tiv {
namespace {
// Version 1 is all that wayland-protocols 1.41 has, and nothing newer is used.
constexpr std::uint32_t VERSION = 1;
constexpr float MIN_LUMINANCE_SCALE = 10000.0F;

WaylandColor *self(void *data) {
    return static_cast<WaylandColor *>(data);
}
}

// The listener tables of libwayland, which reach the private handlers.
struct Events {
    static constexpr wl_registry_listener REGISTRY{
            .global = [](void *data,
                         wl_registry *registry,
                         std::uint32_t name,
                         const char *interface,
                         std::uint32_t version) { self(data)->on_global(registry, name, interface, version); },
            .global_remove = [](void *, wl_registry *, std::uint32_t) {},
    };

    static constexpr wp_color_manager_v1_listener MANAGER{
            .supported_intent =
                    [](void *data, wp_color_manager_v1 *, std::uint32_t intent) { self(data)->on_intent(intent); },
            .supported_feature =
                    [](void *data, wp_color_manager_v1 *, std::uint32_t feature) { self(data)->on_feature(feature); },
            .supported_tf_named = [](void *data, wp_color_manager_v1 *, std::uint32_t tf) { self(data)->on_tf(tf); },
            .supported_primaries_named = [](void *data,
                                            wp_color_manager_v1 *,
                                            std::uint32_t primaries) { self(data)->on_primaries(primaries); },
            .done = [](void *, wp_color_manager_v1 *) {},
    };

    static constexpr wp_color_management_surface_feedback_v1_listener FEEDBACK{
            .preferred_changed = [](void *data,
                                    wp_color_management_surface_feedback_v1 *,
                                    std::uint32_t) { self(data)->on_changed(); },
    };

    static constexpr wp_image_description_v1_listener DESCRIPTION{
            .failed =
                    [](void *data, wp_image_description_v1 *, std::uint32_t, const char *) { self(data)->on_failed(); },
            .ready = [](void *data, wp_image_description_v1 *, std::uint32_t) { self(data)->on_ready(); },
    };

    static constexpr wp_image_description_info_v1_listener INFO{
            .done = [](void *data, wp_image_description_info_v1 *) { self(data)->on_info_done(); },
            .icc_file =
                    [](void *, wp_image_description_info_v1 *, std::int32_t fd, std::uint32_t) {
                        static_cast<void>(::close(fd));
                    },
            .primaries = [](void *,
                            wp_image_description_info_v1 *,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t,
                            std::int32_t) {},
            .primaries_named = [](void *, wp_image_description_info_v1 *, std::uint32_t) {},
            .tf_power = [](void *, wp_image_description_info_v1 *, std::uint32_t) {},
            .tf_named =
                    [](void *data, wp_image_description_info_v1 *, std::uint32_t tf) { self(data)->on_info_tf(tf); },
            .luminances = [](void *data,
                             wp_image_description_info_v1 *,
                             std::uint32_t minimum,
                             std::uint32_t maximum,
                             std::uint32_t reference) { self(data)->on_luminances(minimum, maximum, reference); },
            .target_primaries = [](void *,
                                   wp_image_description_info_v1 *,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t,
                                   std::int32_t) {},
            .target_luminance =
                    [](void *data, wp_image_description_info_v1 *, std::uint32_t minimum, std::uint32_t maximum) {
                        self(data)->on_target(minimum, maximum);
                    },
            .target_max_cll = [](void *, wp_image_description_info_v1 *, std::uint32_t) {},
            .target_max_fall = [](void *, wp_image_description_info_v1 *, std::uint32_t) {},
    };
};

std::unique_ptr<WaylandColor> WaylandColor::create(SDL_Window *window) {
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    auto *display = static_cast<wl_display *>(
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto *surface = static_cast<wl_surface *>(
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

    if (display == nullptr || surface == nullptr) {
        return nullptr;
    }

    std::unique_ptr<WaylandColor> colour(new WaylandColor());

    colour->_display = display;
    colour->_surface = surface;
    // A queue of its own, so dispatching it never runs the handlers of SDL.
    colour->_queue = wl_display_create_queue(display);

    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): every Wayland object is a wl_proxy underneath.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), colour->_queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &Events::REGISTRY, colour.get());

    // The first round trip finds the manager. The second brings what it supports.
    wl_display_roundtrip_queue(display, colour->_queue);
    wl_display_roundtrip_queue(display, colour->_queue);
    wl_registry_destroy(registry);

    if (colour->_manager == nullptr || !colour->usable()) {
        return nullptr;
    }

    colour->_feedback = wp_color_manager_v1_get_surface_feedback(colour->_manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(colour->_feedback, &Events::FEEDBACK, colour.get());

    return colour;
}

WaylandColor::~WaylandColor() {
    forget();

    if (_feedback != nullptr) {
        wp_color_management_surface_feedback_v1_destroy(_feedback);
    }

    if (_manager != nullptr) {
        wp_color_manager_v1_destroy(_manager);
    }

    if (_queue != nullptr) {
        wl_display_flush(_display);
        wl_event_queue_destroy(_queue);
    }
}

void WaylandColor::on_global(wl_registry *registry,
                             const std::uint32_t name,
                             const char *interface,
                             const std::uint32_t version) {
    if (std::string_view(interface) != wp_color_manager_v1_interface.name || _manager != nullptr) {
        return;
    }

    _manager = static_cast<wp_color_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min(version, VERSION)));
    wp_color_manager_v1_add_listener(_manager, &Events::MANAGER, this);
}

void WaylandColor::on_feature(const std::uint32_t feature) {
    _parametric = _parametric || feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC;
    _luminances = _luminances || feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES;
    _mastering = _mastering || feature == WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES;
}

void WaylandColor::on_tf(const std::uint32_t tf) {
    _pq = _pq || tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
}

void WaylandColor::on_primaries(const std::uint32_t primaries) {
    _bt2020 = _bt2020 || primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
}

void WaylandColor::on_intent(const std::uint32_t intent) {
    _perceptual = _perceptual || intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL;
}

void WaylandColor::on_luminances(const std::uint32_t minimum,
                                 const std::uint32_t maximum,
                                 const std::uint32_t reference) {
    _infoMinimum = static_cast<float>(minimum) / MIN_LUMINANCE_SCALE;
    _infoMaximum = static_cast<float>(maximum);
    _infoReference = static_cast<float>(reference);
}

void WaylandColor::on_target(const std::uint32_t minimum, const std::uint32_t maximum) {
    _targetMinimum = static_cast<float>(minimum) / MIN_LUMINANCE_SCALE;
    _targetMaximum = static_cast<float>(maximum);
    _targetKnown = true;
}

bool WaylandColor::usable() const {
    return _parametric && _luminances && _pq && _bt2020 && _perceptual;
}

// Two round trips: one for the description to be ready, one for what it holds.
void WaylandColor::read_output() {
    _changed = false;
    _ready = false;
    _failed = false;
    _infoDone = false;
    _infoTf = 0;
    _infoMinimum = 0.0F;
    _infoMaximum = 0.0F;
    _infoReference = 0.0F;
    _targetMinimum = 0.0F;
    _targetMaximum = 0.0F;
    _targetKnown = false;

    wp_image_description_v1 *preferred = wp_color_management_surface_feedback_v1_get_preferred_parametric(_feedback);

    wp_image_description_v1_add_listener(preferred, &Events::DESCRIPTION, this);

    while (!_ready && !_failed && wl_display_roundtrip_queue(_display, _queue) >= 0) {
    }

    if (_ready) {
        wp_image_description_info_v1 *info = wp_image_description_v1_get_information(preferred);

        wp_image_description_info_v1_add_listener(info, &Events::INFO, this);

        while (!_infoDone && wl_display_roundtrip_queue(_display, _queue) >= 0) {
        }

        wp_image_description_info_v1_destroy(info);
    }

    wp_image_description_v1_destroy(preferred);

    // The target volume is what the panel shows, where the primary volume only bounds the encoding.
    const float peak = _targetKnown && _targetMaximum > 0.0F ? _targetMaximum : _infoMaximum;

    _output = {
            .minimum = _targetKnown ? _targetMinimum : _infoMinimum,
            .peak = peak,
            .reference = _infoReference > 0.0F ? _infoReference : Tone::PQ_REFERENCE_NITS,
            .hdr = _infoDone && _infoTf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ && peak > _infoReference,
    };
}

const WaylandColor::Output &WaylandColor::output() {
    wl_display_dispatch_queue_pending(_display, _queue);

    if (_changed) {
        read_output();
    }

    return _output;
}

bool WaylandColor::describe(const float minimum, const float peak) {
    // The description belongs to the surface, so a swapchain remade for the same display needs no new one.
    if (_described != nullptr && minimum == _describedMinimum && peak == _describedPeak) {
        return true;
    }

    if (_described == nullptr) {
        _described = wp_color_manager_v1_get_surface(_manager, _surface);
    }

    wp_image_description_creator_params_v1 *creator = wp_color_manager_v1_create_parametric_creator(_manager);
    const auto low = static_cast<std::uint32_t>(std::max(minimum, 0.0F) * MIN_LUMINANCE_SCALE);
    const auto high = static_cast<std::uint32_t>(std::max(peak, minimum + 1.0F));

    wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    // The protocol has PQ ignore this maximum, but Hyprland tone maps whatever lies above it. The tone
    // mapper of tinyiv rolls highlights off at the peak already.
    wp_image_description_creator_params_v1_set_luminances(
            creator, low, high, static_cast<std::uint32_t>(Tone::PQ_REFERENCE_NITS));

    if (_mastering) {
        wp_image_description_creator_params_v1_set_mastering_luminance(creator, low, high);
    }

    wp_image_description_v1 *description = wp_image_description_creator_params_v1_create(creator);

    _ready = false;
    _failed = false;
    wp_image_description_v1_add_listener(description, &Events::DESCRIPTION, this);

    while (!_ready && !_failed && wl_display_roundtrip_queue(_display, _queue) >= 0) {
    }

    if (_ready) {
        wp_color_management_surface_v1_set_image_description(
                _described, description, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        _describedMinimum = minimum;
        _describedPeak = peak;
    }

    wp_image_description_v1_destroy(description);
    wl_display_flush(_display);

    return _ready;
}

void WaylandColor::forget() {
    if (_described == nullptr) {
        return;
    }

    wp_color_management_surface_v1_destroy(_described);
    _described = nullptr;
    _describedMinimum = 0.0F;
    _describedPeak = 0.0F;
    wl_display_flush(_display);
}
}
#else
namespace tiv {
// Off Linux the window never has a Wayland surface to describe.
std::unique_ptr<WaylandColor> WaylandColor::create([[maybe_unused]] SDL_Window *window) {
    return nullptr;
}

WaylandColor::~WaylandColor() = default;

const WaylandColor::Output &WaylandColor::output() {
    return _output;
}

bool WaylandColor::describe([[maybe_unused]] const float minimum, [[maybe_unused]] const float peak) {
    return false;
}

void WaylandColor::forget() {
}
}
#endif
