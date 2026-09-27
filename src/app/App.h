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
#include "gallery/Folder.h"
#include "image/Decode.h"
#include "render/Canvas.h"
#include "services/Loader.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv {
    // The window, the loop, and the wiring between folder, loader, view and renderer.
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

    private:
        bool open_window(std::string *error);
        void layout();
        void set_fullscreen(bool on);

        // Direction is the way the wheel was going, so prefetch leans that way.
        void show(int index, int direction);
        void handle(const SDL_Event &event);
        bool handle_play_bar(const SDL_Event &event);
        void seek_to(double x);
        void step_frame(int delta);
        void deliver();
        void deliver_detail();
        void warm_neighbours();
        // The view changed, so the part on screen may want rendering again once it rests.
        void moved();
        void refine();
        void frame();

        // Milliseconds the loop may sleep, or -1 for until the next event.
        [[nodiscard]] int wait_ms() const;
        [[nodiscard]] Rect play_bar() const;

        [[nodiscard]] std::string bar_left() const;
        [[nodiscard]] std::string bar_right() const;

        // What was last sent to the refiner, so a request that failed is not repeated.
        struct Ask {
            std::uint64_t generation = 0;
            double scale = 0.0;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;

            bool operator==(const Ask &) const = default;
        };

        SDL_Window *_window = nullptr;
        SDL_Renderer *_renderer = nullptr;
        std::uint32_t _loaderEvent = 0;
        std::uint32_t _refinerEvent = 0;

        Folder _folder;
        Viewport _viewport;
        Playback _playback;
        Input _input;
        std::unique_ptr<Canvas> _canvas;
        std::unique_ptr<Loader> _loader;
        std::unique_ptr<Refiner> _refiner;

        Decode::Info _info;
        std::uintmax_t _bytes = 0;
        std::uint64_t _generation = 0;
        // Whose levels the canvas holds.
        std::uint64_t _shown = 0;
        // Something of the current image is still on its way: its first pixels, or the whole
        // image behind a preview.
        bool _loading = false;
        int _direction = 1;
        // Shown in place of an image that could not be opened.
        std::string _failure;
        // A drag that started on the play bar's track.
        bool _seeking = false;

        std::uint64_t _movedAt = 0;
        // Moved and not yet looked at by refine().
        bool _settling = false;
        Ask _asked;

        bool _fullscreen = false;
        bool _running = true;
        bool _dirty = true;
        int _repaints = 0;
    };
}


#endif //TIV_APP_APP_H
