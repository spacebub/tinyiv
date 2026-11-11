// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_APP_H
#define TIV_APP_APP_H


#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "app/Input.h"
#include "app/Picture.h"
#include "app/Window.h"
#include "gallery/Folder.h"
#include "image/decode/Decode.h"
#include "services/Loader.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv {
class App {

public:
    // Bytes of texture uploaded per frame at most.
    static constexpr std::size_t UPLOAD_BUDGET = std::size_t{32} * 1024 * 1024;

    // Frames drawn after a window event, since the first can land before the compositor has caught up.
    static constexpr int REPAINTS = 2;

    App() = default;
    ~App();

    App(const App &) = delete;
    App(App &&) = delete;
    App &operator=(const App &) = delete;
    App &operator=(App &&) = delete;

    // An empty file opens the window with nothing in it, until a file is dropped on it.
    bool start(const std::filesystem::path &file, std::string *error = nullptr);
    void run();

    // What a driver outside the loop sees between frames.
    struct State {
        std::filesystem::path file;
        int index = 0;
        int count = 0;
        Folder::Order order = Folder::Order::AToZ;
        Decode::Info info;
        // The orientation in the file composed with any turn not saved.
        int orientation = 1;
        // Nothing is still to decode, read, upload, render again or draw.
        bool settled = false;
        bool failed = false;
        bool streamed = false;
        bool streaming = false;
        bool linear = false;
        bool animated = false;
        bool playing = false;
        std::size_t frame = 0;
        double zoom = 0.0;
        float progress = 0.0F;
        bool help = false;
        bool fullscreen = false;
        bool bars = false;
        std::string message;
    };

    // Main thread only.
    [[nodiscard]] State state() const;

private:
    void layout();
    void update_display();
    [[nodiscard]] bool bar_shown() const;
    // The top and the bottom bar share this height. It is zero while they are hidden.
    [[nodiscard]] int bar_height() const;
    void toggle_bar();

    void deliver();
    void handle(const SDL_Event &event);
    bool handle_play_bar(const SDL_Event &event);
    bool handle_help(const SDL_Event &event);
    void seek_to(double x);
    void step_frame(int delta);
    void frame();

    void wait_for_event();
    // Milliseconds the loop may sleep, or -1 for until the next event.
    [[nodiscard]] int wait_ms() const;
    // Milliseconds until the frame wanted may be drawn, or -1 when no frame is wanted.
    [[nodiscard]] int frame_wait_ms() const;
    [[nodiscard]] Rect play_bar() const;

    Window _window;
    std::uint32_t _loaderEvent = 0;
    std::uint32_t _refinerEvent = 0;
    // A tile cache read a tile the view asked for.
    std::uint32_t _tileCacheEvent = 0;

    Folder _folder;
    Viewport _viewport;
    Playback _playback;
    Input _input;
    Picture _picture;
    std::unique_ptr<Loader> _loader;
    std::unique_ptr<Refiner> _refiner;

    bool _help = false;
    // A drag that started on the play bar's track.
    bool _seeking = false;

    // Whether the bars show, kept apart for windowed and fullscreen: I switches the one in use.
    bool _barWindowed = true;
    bool _barFullscreen = false;
    bool _running = true;
    bool _dirty = true;
    int _repaints = 0;
    // In SDL_GetTicksNS() time.
    std::uint64_t _drawnAt = 0;
};
}


#endif //TIV_APP_APP_H
