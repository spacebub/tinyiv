// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_WAYLANDCOLOR_H
#define TIV_RENDER_WAYLANDCOLOR_H


#include <cstdint>
#include <memory>

#include <SDL3/SDL.h>

struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_surface;
struct wp_color_manager_v1;
struct wp_color_management_surface_v1;
struct wp_color_management_surface_feedback_v1;

namespace tiv {
// Describes the colour of the window surface to a Wayland compositor while HDR is shown. The
// swapchain then passes PQ through untouched, and the description carries the real luminances.
class WaylandColor {

public:
    struct Output {
        float minimum = 0.0F;
        float peak = 0.0F;
        float reference = 0.0F;
        bool hdr = false;
    };

    // Null unless the window is on Wayland and the compositor takes PQ descriptions with luminances.
    [[nodiscard]] static std::unique_ptr<WaylandColor> create(SDL_Window *window);
    ~WaylandColor();

    WaylandColor(const WaylandColor &) = delete;
    WaylandColor(WaylandColor &&) = delete;
    WaylandColor &operator=(const WaylandColor &) = delete;
    WaylandColor &operator=(WaylandColor &&) = delete;

    // What the compositor prefers for the window. Read again after it reports a change.
    [[nodiscard]] const Output &output();

    // Call only while the swapchain is a pass-through one. Mesa describes every other swapchain,
    // and a second description closes the window.
    // False when the compositor refused the description.
    bool describe(float minimum, float peak);
    void forget();

private:
    friend struct Events;

    void on_feature(std::uint32_t feature);
    void on_tf(std::uint32_t tf);
    void on_primaries(std::uint32_t primaries);
    void on_intent(std::uint32_t intent);
    void on_changed() { _changed = true; }
    void on_ready() { _ready = true; }
    void on_failed() { _failed = true; }
    void on_luminances(std::uint32_t minimum, std::uint32_t maximum, std::uint32_t reference);
    void on_target(std::uint32_t minimum, std::uint32_t maximum);
    void on_info_tf(std::uint32_t tf) { _infoTf = tf; }
    void on_info_done() { _infoDone = true; }
    void on_global(wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version);

    WaylandColor() = default;

    [[nodiscard]] bool usable() const;
    void read_output();

    wl_display *_display = nullptr;
    wl_surface *_surface = nullptr;
    wl_event_queue *_queue = nullptr;
    wp_color_manager_v1 *_manager = nullptr;
    wp_color_management_surface_feedback_v1 *_feedback = nullptr;
    wp_color_management_surface_v1 *_described = nullptr;

    bool _parametric = false;
    bool _luminances = false;
    bool _mastering = false;
    bool _pq = false;
    bool _bt2020 = false;
    bool _perceptual = false;

    bool _changed = true;
    bool _ready = false;
    bool _failed = false;
    bool _infoDone = false;
    std::uint32_t _infoTf = 0;
    float _infoMinimum = 0.0F;
    float _infoMaximum = 0.0F;
    float _infoReference = 0.0F;
    float _targetMinimum = 0.0F;
    float _targetMaximum = 0.0F;
    bool _targetKnown = false;
    float _describedMinimum = 0.0F;
    float _describedPeak = 0.0F;
    Output _output;
};
}


#endif //TIV_RENDER_WAYLANDCOLOR_H
