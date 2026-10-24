// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_HARNESS_DRIVER_H
#define TIV_HARNESS_DRIVER_H


#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "Corpus.h"
#include "app/App.h"

namespace harness {
    // Records the time of every step in a sitting and each check that failed.
    struct Report {
        struct Step {
            std::string name;
            double seconds = 0.0;
        };

        std::vector<Step> steps;
        std::vector<std::string> failed;
    };

    // Runs the app as a person would. The app loop keeps the main thread. A script on a second
    // thread sends keys, wheel turns, clicks and drops, then waits for the app to settle. The
    // driver reads the app state on the main thread between frames.
    class Driver {

    public:
        using Script = std::function<void(Driver &)>;

        // Seconds a step may wait for the app to settle before it counts as stuck.
        Driver(Report &report, double patience);

        // Starts the app on the file, or with no file when the path is empty. The script then
        // runs against the app, and the app quits once the script ends.
        void session(const std::filesystem::path &file, const Script &script);

        // The rest is for the script thread.

        [[nodiscard]] tiv::App::State state();

        // Waits until nothing is left to decode, read, upload or draw. Every event sent must be
        // handled too. The time goes into the report under the step name. Prints progress while
        // it waits. Returns false when the app stopped moving before it settled.
        bool settle(std::string_view step);

        void key(SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE);
        // Positive notches go to the next image, as the wheel turned down does.
        void wheel(int notches);
        // Window points. Moves are spread evenly along the way.
        void drag(std::uint8_t button, float x, float y, float dx, float dy, int moves);
        void click(float x, float y, int clicks = 1);
        void drop(const std::filesystem::path &file);
        void resize(int width, int height);
        // The window in points.
        [[nodiscard]] SDL_Point size();
        // Lets time pass, for animations to play.
        static void pause(int ms);

        void check(bool ok, const std::string &what);
        // Checks that the image on screen is the entry and shows as the entry says.
        void expect(const Entry &entry);

    private:
        // Thrown on the script thread once the app has quit. It ends the script.
        struct Ended : std::exception {};

        // Asks the app to quit unless it has already. Throws nothing.
        void quit();
        void on_main(const std::function<void()> &work);
        void push(SDL_Event &event);

        Report &_report;
        double _patience;
        tiv::App *_app = nullptr;
        SDL_WindowID _window = 0;
        std::atomic<bool> _ended = false;
        // Kept until the drop is handled, as the event points into it.
        std::string _dropped;
    };
}


#endif //TIV_HARNESS_DRIVER_H
