// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_WINDOW_H
#define TIV_APP_WINDOW_H


#include <memory>
#include <optional>
#include <string>

#include <SDL3/SDL.h>

#include "app/Config.h"
#include "image/Tone.h"
#include "render/Canvas.h"
#include "render/Painter.h"
#include "render/SdlPainter.h"
#include "render/gpu/VulkanPainter.h"
#include "services/Loader.h"

namespace tiv {
class Window {

public:
    struct Size {
        int width = 0;
        int height = 0;
    };

    Window() = default;
    ~Window();

    Window(const Window &) = delete;
    Window(Window &&) = delete;
    Window &operator=(const Window &) = delete;
    Window &operator=(Window &&) = delete;

    bool open(const Config &config, std::string *error);
    bool make_renderer(bool linear);
    void follow_output(const Loader::Result &result);
    void set_fullscreen(bool on);
    bool update_display();
    void update_refresh();

    [[nodiscard]] Size pixels() const;
    [[nodiscard]] std::optional<Size> desktop_pixels() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] float density() const;
    [[nodiscard]] float color_scale() const;
    // Null once drawing has failed for good.
    [[nodiscard]] Painter *painter() const;
    [[nodiscard]] Canvas &canvas() const { return *_canvas; }
    [[nodiscard]] const Tone::Display &display() const { return _display; }
    [[nodiscard]] bool hdr10() const { return _hdr10; }
    [[nodiscard]] bool fullscreen() const { return _fullscreen; }
    [[nodiscard]] bool vsync() const;
    [[nodiscard]] bool lost() const { return _vulkan != nullptr && _vulkan->lost(); }
    [[nodiscard]] float refresh_rate() const { return _refreshRate; }

private:
    void follow_awake() const;
    [[nodiscard]] bool make_vulkan(std::string *error);
    bool update_vulkan_display();

    SDL_Window *_window = nullptr;
    SDL_Renderer *_renderer = nullptr;
    // Shared by each renderer the window has, null where the gpu renderer is not available.
    SDL_GPUDevice *_device = nullptr;
    // Set by TIV_RENDERER=own while both renderers exist.
    std::unique_ptr<gpu::VulkanPainter> _vulkan;
    std::unique_ptr<SdlPainter> _sdl;
    std::unique_ptr<Canvas> _canvas;
    Tone::Display _display;
    float _hdrWhite = 100.0F;
    bool _lean = false;
    bool _hdr10 = false;
    bool _fullscreen = false;
    Config::KeepAwake _awake = Config::KeepAwake::Fullscreen;
    // Presentation waits for the display. Without it, each frame of a drag comes at least half a refresh
    // after the one before.
    bool _vsync = true;
    float _refreshRate = 60.0F;
};
}


#endif //TIV_APP_WINDOW_H
