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
#include "image/Tone.h"
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
        struct Size {
            int width = 0;
            int height = 0;
        };

        bool open_window(std::string *error);
        void layout();
        void update_display();
        bool make_renderer(bool linear);
        void follow_output(const Loader::Result &result);
        void set_fullscreen(bool on);

        // Direction is the way the wheel was going, so prefetch leans that way.
        void show(int index, int direction);
        void handle(const SDL_Event &event);
        bool handle_play_bar(const SDL_Event &event);
        bool handle_help(const SDL_Event &event);
        void seek_to(double x);
        void step_frame(int delta);
        void deliver();
        void deliver_detail();
        void give_back();
        void warm_neighbours();
        // Reads the image from disk again, dropping any turn not saved.
        void reload();
        // Composes a turn or flip with how the image is shown.
        void turn(int by);
        // Writes how the image is turned into its file, when it is turned.
        void save();
        // Switches streaming mode, where every still image shows from a pyramid on disk.
        void toggle_stream();
        // How the image on screen is drawn, from how it is stored.
        [[nodiscard]] int orientation() const;
        // The size of the image on screen as stored, and as shown.
        [[nodiscard]] Size stored() const;
        [[nodiscard]] Size shown() const;
        // The view changed, so the part on screen may want rendering again once it rests.
        void moved();
        void refine();
        void frame();

        void wait_for_event();
        // Milliseconds the loop may sleep, or -1 for until the next event.
        [[nodiscard]] int wait_ms() const;
        [[nodiscard]] Rect play_bar() const;

        // Tagged leads with the streaming mode when it is on.
        [[nodiscard]] std::string bar_left(bool tagged = true) const;
        [[nodiscard]] std::string failure_text(const std::string &error) const;
        // Fullscreen shows the streaming mode for a moment after it switches.
        [[nodiscard]] bool flashing() const;
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
        // Shared by each renderer the window has, null where the gpu renderer is not available.
        SDL_GPUDevice *_device = nullptr;
        std::uint32_t _loaderEvent = 0;
        std::uint32_t _refinerEvent = 0;
        // A store read a tile the view asked for.
        std::uint32_t _storeEvent = 0;
        Tone::Display _display;

        Folder _folder;
        Viewport _viewport;
        Playback _playback;
        Input _input;
        std::unique_ptr<Canvas> _canvas;
        std::unique_ptr<Loader> _loader;
        std::unique_ptr<Refiner> _refiner;

        Decode::Info _info;
        // Turns and flips on top of the orientation the file says, until saved.
        int _turn = 1;
        // Replaces the image details in the status bar until the next image or turn.
        std::string _message;
        bool _help = false;
        // Images came and went since freed memory was last given back.
        bool _giveBack = false;
        std::uint64_t _shownAt = 0;
        std::uintmax_t _bytes = 0;
        std::uint64_t _generation = 0;
        // Whose levels the canvas holds.
        std::uint64_t _shown = 0;
        // Something of the current image is still on its way: its first pixels, or the whole
        // image behind a preview.
        bool _loading = false;
        // The image is too large for memory, and its tiles are being written to disk.
        bool _building = false;
        // The image shows from disk.
        bool _streamed = false;
        // The renderer draws in linear light, for an HDR image.
        bool _linear = false;
        int _direction = 1;
        // Shown in place of an image that could not be opened.
        std::string _failure;
        // A drag that started on the play bar's track.
        bool _seeking = false;

        std::uint64_t _movedAt = 0;
        // When streaming mode last switched, zero before it ever has.
        std::uint64_t _switchedAt = 0;
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
