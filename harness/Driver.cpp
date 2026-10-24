// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <SDL3/SDL.h>

#include "Corpus.h"
#include "Driver.h"
#include "app/App.h"

namespace harness {
    namespace {
        using Clock = std::chrono::steady_clock;

        // The wait between two looks at the app. Each look wakes the app loop, and that shows in a profile.
        constexpr std::chrono::milliseconds POLL{5};
        // Settled this many looks in a row, so a state caught between two events does not count.
        constexpr int STEADY = 2;
        constexpr double REPORT_EVERY = 2.0;

        double seconds_since(const Clock::time_point start) {
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        // The app moved when the image or the build progress changed.
        bool moved(const tiv::App::State &before, const tiv::App::State &after) {
            return before.file != after.file || before.progress != after.progress || before.streamed != after.streamed
                   || before.failed != after.failed || before.info.width != after.info.width
                   || before.frame != after.frame;
        }
    }

    Driver::Driver(Report &report, const double patience) : _report(report), _patience(patience) {
    }

    void Driver::session(const std::filesystem::path &file, const Script &script) {
        tiv::App app;
        std::string error;

        if (!app.start(file, &error)) {
            check(false, "the app did not start: " + error);

            return;
        }

        int count = 0;
        SDL_Window **windows = SDL_GetWindows(&count);

        _window = count > 0 ? SDL_GetWindowID(std::span(windows, static_cast<std::size_t>(count)).front()) : 0;
        SDL_free(static_cast<void *>(windows));
        _app = &app;
        _ended = false;

        std::atomic<bool> done = false;
        std::thread runner([&] {
            try {
                script(*this);
            } catch (const Ended &) {
                check(false, "the app quit before the script ended");
            } catch (const std::exception &trouble) {
                check(false, std::string("the script stopped: ") + trouble.what());
            }

            quit();
            done = true;
        });

        app.run();
        _ended = true;

        // The script may still wait on a look at the app. Looks only run while events are pumped.
        while (!done) {
            SDL_PumpEvents();
            std::this_thread::sleep_for(POLL);
        }

        runner.join();
        _app = nullptr;
    }

    void Driver::quit() {
        if (_ended) {
            return;
        }

        SDL_Event event{};

        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }

    void Driver::on_main(const std::function<void()> &work) {
        if (_ended) {
            throw Ended{};
        }

        std::function<void()> call = work;
        const auto run = [](void *data) { (*static_cast<std::function<void()> *>(data))(); };

        if (!SDL_RunOnMainThread(run, &call, true)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    tiv::App::State Driver::state() {
        tiv::App::State now;

        on_main([&] { now = _app->state(); });

        return now;
    }

    bool Driver::settle(const std::string_view step) {
        const Clock::time_point start = Clock::now();
        Clock::time_point lastMove = start;
        double reported = 0.0;
        tiv::App::State last;
        int steady = 0;

        while (steady < STEADY) {
            tiv::App::State now;
            bool quiet = false;

            on_main([&] {
                now = _app->state();
                quiet = !SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            });

            steady = now.settled && quiet ? steady + 1 : 0;

            if (moved(last, now)) {
                lastMove = Clock::now();
            }

            last = now;

            if (seconds_since(lastMove) > _patience) {
                check(false, std::format("{}: nothing moved for {:.0f} s on {}", step, _patience,
                                         now.file.filename().string()));

                return false;
            }

            if (const double waited = seconds_since(start); waited - reported >= REPORT_EVERY) {
                reported = waited;
                std::println("    {} waiting {:.0f} s on {}{}", step, waited, now.file.filename().string(),
                             now.progress > 0.0F && now.progress < 1.0F
                                     ? std::format(", tiles {:.0f}%", now.progress * 100.0F)
                                     : std::string());
            }

            if (steady < STEADY) {
                std::this_thread::sleep_for(POLL);
            }
        }

        _report.steps.push_back({.name = std::string(step), .seconds = seconds_since(start)});

        return true;
    }

    void Driver::push(SDL_Event &event) {
        if (_ended) {
            throw Ended{};
        }

        if (!SDL_PushEvent(&event)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    void Driver::key(const SDL_Keycode key, const SDL_Keymod mod) {
        SDL_Event event{};

        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = _window;
        event.key.key = key;
        event.key.mod = mod;
        event.key.down = true;
        push(event);

        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        push(event);
    }

    void Driver::wheel(const int notches) {
        const SDL_Point window = size();
        SDL_Event event{};

        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.windowID = _window;
        event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        event.wheel.integer_y = -notches;
        event.wheel.y = static_cast<float>(-notches);
        event.wheel.mouse_x = static_cast<float>(window.x) / 2.0F;
        event.wheel.mouse_y = static_cast<float>(window.y) / 2.0F;
        push(event);
    }

    void Driver::drag(const std::uint8_t button, const float x, const float y, const float dx, const float dy,
                      const int moves) {
        SDL_Event event{};

        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = _window;
        event.button.button = button;
        event.button.down = true;
        event.button.clicks = 1;
        event.button.x = x;
        event.button.y = y;
        push(event);

        const float stepX = dx / static_cast<float>(moves);
        const float stepY = dy / static_cast<float>(moves);

        for (int i = 1; i <= moves; ++i) {
            SDL_Event motion{};

            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = _window;
            motion.motion.state = SDL_BUTTON_MASK(button);
            motion.motion.x = x + (stepX * static_cast<float>(i));
            motion.motion.y = y + (stepY * static_cast<float>(i));
            motion.motion.xrel = stepX;
            motion.motion.yrel = stepY;
            push(motion);
        }

        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.down = false;
        event.button.x = x + dx;
        event.button.y = y + dy;
        push(event);
    }

    void Driver::click(const float x, const float y, const int clicks) {
        SDL_Event event{};

        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = _window;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.clicks = static_cast<std::uint8_t>(clicks);
        event.button.x = x;
        event.button.y = y;
        push(event);

        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.down = false;
        push(event);
    }

    void Driver::drop(const std::filesystem::path &file) {
        _dropped = file.string();

        SDL_Event event{};

        event.type = SDL_EVENT_DROP_FILE;
        event.drop.windowID = _window;
        event.drop.data = _dropped.c_str();
        push(event);
    }

    void Driver::resize(const int width, const int height) {
        on_main([&] {
            SDL_Window *window = SDL_GetWindowFromID(_window);

            SDL_SetWindowFullscreen(window, false);
            SDL_SetWindowSize(window, width, height);
            SDL_SyncWindow(window);
        });
    }

    SDL_Point Driver::size() {
        SDL_Point points{};

        on_main([&] { SDL_GetWindowSize(SDL_GetWindowFromID(_window), &points.x, &points.y); });

        return points;
    }

    void Driver::pause(const int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void Driver::check(const bool ok, const std::string &what) {
        if (!ok) {
            std::println("    FAILED {}", what);
            _report.failed.push_back(what);
        }
    }

    void Driver::expect(const Entry &entry) {
        const tiv::App::State now = state();
        const std::string name = entry.file.filename().string();

        if (now.file.filename() != entry.file.filename()) {
            check(false, std::format("expected {} on screen, found {}", name, now.file.filename().string()));

            return;
        }

        if (entry.broken) {
            check(now.failed, name + " opened but should not have");

            return;
        }

        check(!now.failed, name + " did not open");
        check(now.info.format == entry.format,
              std::format("{} shows as {}, not {}", name, now.info.format, entry.format));
        check(now.info.kind == entry.kind, name + " was taken for another format");
        check(now.info.width == entry.width && now.info.height == entry.height,
              std::format("{} shows {}x{}, not {}x{}", name, now.info.width, now.info.height, entry.width,
                          entry.height));
        check(now.info.orientation == entry.orientation,
              std::format("{} has orientation {}, not {}", name, now.info.orientation, entry.orientation));
        check(now.info.hdr == entry.hdr, std::format("{} {} HDR", name, entry.hdr ? "is not" : "is"));
        check(now.animated == (entry.frames > 1),
              std::format("{} {} as an animation", name, entry.frames > 1 ? "does not play" : "plays"));
    }
}
