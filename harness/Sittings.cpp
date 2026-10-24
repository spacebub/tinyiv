// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "Corpus.h"
#include "Driver.h"
#include "Sandbox.h"
#include "Sittings.h"
#include "app/App.h"
#include "gallery/Folder.h"

namespace harness {
    namespace {
        constexpr int WINDOW_WIDTH = 1600;
        constexpr int WINDOW_HEIGHT = 1000;
        // Half the height of the play bar. With the bars hidden, the play bar runs along the bottom.
        constexpr float PLAY_BAR_MIDDLE = 12.0F;
        // Notches of a wheel flick, faster than the images decode.
        constexpr int FLICK = 12;
        constexpr int FLICK_PAUSE_MS = 15;
        constexpr int ORDERS = 5;
        constexpr int PLAY_MS = 600;
        constexpr int PLAY_SAMPLE_MS = 10;

        struct Settings {
            std::filesystem::path cache;
            std::string_view cacheMode = "fast";
            bool streaming = false;
            bool persist = false;
            bool footprint = false;
            std::string_view sort = "a-z";
        };

        void configure(const Settings &settings) {
            Sandbox::write_config(std::format("cache = \"{}\"\ncache_mode = {}\nstreaming_mode = {}\n"
                                              "persist_streams = {}\nmemory_mode = {}\nsort = {}\n",
                                              settings.cache.string(), settings.cacheMode,
                                              settings.streaming ? "on" : "off", settings.persist ? "on" : "off",
                                              settings.footprint ? "footprint" : "speed", settings.sort));
        }

        std::string slurp(const std::filesystem::path &file) {
            std::ifstream in(file, std::ios::binary | std::ios::ate);
            std::string text(static_cast<std::size_t>(std::max<std::streamoff>(in.tellg(), 0)), '\0');

            in.seekg(0);
            in.read(text.data(), static_cast<std::streamsize>(text.size()));

            return text;
        }

        std::size_t files_under(const std::filesystem::path &dir) {
            std::error_code failure;
            std::size_t count = 0;

            for (auto it = std::filesystem::recursive_directory_iterator(dir, failure);
                 !failure && it != std::filesystem::recursive_directory_iterator(); it.increment(failure)) {
                count += it->is_regular_file() ? 1 : 0;
            }

            return count;
        }

        void open(Driver &driver) {
            driver.resize(WINDOW_WIDTH, WINDOW_HEIGHT);
            driver.settle("open");
        }

        void shown(const Library &library, Driver &driver) {
            const tiv::App::State state = driver.state();
            const Entry *entry = library.find(state.file);

            driver.check(entry != nullptr, state.file.string() + " is not in the corpus");

            if (entry != nullptr) {
                driver.expect(*entry);
            }
        }

        // Shows every image of the folder in turn from the first. Each one is checked, then handed to each.
        void walk(const Library &library, Driver &driver, const std::string_view step,
                  const std::function<void()> &each = {}) {
            driver.key(SDLK_HOME);
            driver.settle(step);

            const int count = driver.state().count;

            for (int i = 0; i < count; ++i) {
                if (i > 0) {
                    driver.key(SDLK_RIGHT);
                    driver.settle(step);
                }

                shown(library, driver);

                if (each) {
                    each();
                }
            }
        }

        // Actual size, a pan both ways, zoom steps and a zoom drag, then fit again.
        void look_around(Driver &driver) {
            const SDL_Point window = driver.size();
            const float x = static_cast<float>(window.x) / 2.0F;
            const float y = static_cast<float>(window.y) / 2.0F;
            constexpr float REACH = 0.9F;
            constexpr int MOVES = 12;

            driver.key(SDLK_2);
            driver.settle("actual size");
            driver.drag(SDL_BUTTON_LEFT, x, y, -x * REACH, -y * REACH, MOVES);
            driver.settle("pan");
            driver.drag(SDL_BUTTON_LEFT, x, y, x * REACH, y * REACH, MOVES);
            driver.settle("pan");
            driver.key(SDLK_UP);
            driver.settle("zoom step");
            driver.key(SDLK_DOWN);
            driver.key(SDLK_DOWN);
            driver.settle("zoom step");
            driver.key(SDLK_0);
            driver.settle("centre");
            driver.drag(SDL_BUTTON_RIGHT, x, y, 0.0F, -y * REACH, MOVES);
            driver.settle("zoom drag");
            driver.key(SDLK_1);
            driver.settle("fit");
        }

        // Steps on before each image has finished, so decodes and tile builds are cut short.
        void hurry(Driver &driver) {
            constexpr int STEP_MS = 40;

            driver.key(SDLK_HOME);
            driver.settle("first");

            const int count = driver.state().count;

            for (int i = 1; i < count; ++i) {
                driver.key(SDLK_RIGHT);
                Driver::pause(STEP_MS);
            }

            driver.settle("hurry");
        }

        std::filesystem::path in_copy(const Library &library, const std::filesystem::path &dir,
                                      const std::string_view folder) {
            return dir / library.any(folder).filename();
        }

        void first_run(const Library &library, Driver &driver) {
            const std::filesystem::path work = Sandbox::fresh("first-run");
            std::error_code failure;

            std::filesystem::remove(Sandbox::config(), failure);

            driver.session({}, [&](Driver &d) {
                d.settle("empty window");
                d.check(d.state().count == 0, "a start without a file shows a folder");
                d.drop(library.any("browse"));
                d.settle("drop");
                shown(library, d);
            });

            const std::string made = slurp(Sandbox::config());

            for (const std::string_view key :
                 {"cache =", "cache_mode =", "streaming_mode =", "persist_streams =", "memory_mode =", "sort ="}) {
                driver.check(made.contains(key), std::format("the config made on first start lacks {}", key));
            }

            // Every setting differs from the default. Lines the config cannot take come first.
            Sandbox::write_config(std::format("not a setting\ncolour = red\nsort = sideways\ncache = \"{}\"\n"
                                              "cache_mode = small\nstreaming_mode = on\npersist_streams = on\n"
                                              "memory_mode = speed\nsort = z-a\n",
                                              (work / "cache").string()));

            driver.session(library.any("browse"), [&](Driver &d) {
                open(d);

                const tiv::App::State state = d.state();

                d.check(state.order == tiv::Folder::Order::ZToA, "sort = z-a was not taken");
                d.check(state.streaming, "streaming_mode = on was not taken");
                walk(library, d, "step");
            });

            driver.check(files_under(work / "cache") > 0, "persist_streams = on left nothing on disk");
        }

        void browse(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("browse") / "cache"});

            driver.session(library.any("browse"), [&](Driver &d) {
                open(d);
                walk(library, d, "step");

                for (const int way : {1, -1}) {
                    for (int i = 0; i < FLICK; ++i) {
                        d.wheel(way);
                        Driver::pause(FLICK_PAUSE_MS);
                    }

                    d.settle("flick");
                    shown(library, d);
                }

                d.key(SDLK_END);
                d.settle("last");
                shown(library, d);
                d.key(SDLK_HOME);
                d.settle("first");

                for (int i = 0; i < ORDERS; ++i) {
                    d.key(SDLK_O);
                    d.settle("sort");
                    d.key(SDLK_RIGHT);
                    d.settle("step");
                    shown(library, d);
                }

                d.check(d.state().order == tiv::Folder::Order::AToZ, "every order in turn did not come back to a-z");
                look_around(d);

                // The right and left turns cancel out. The two flips leave a half turn, and a reload drops it.
                const int before = d.state().orientation;

                d.key(SDLK_RIGHTBRACKET);
                d.key(SDLK_SEMICOLON);
                d.key(SDLK_APOSTROPHE);
                d.key(SDLK_LEFTBRACKET);
                d.settle("turn");
                d.check(d.state().orientation != before, "turns and flips left the image as it was");
                d.key(SDLK_R);
                d.settle("reload");
                d.check(d.state().orientation == before, "a reload kept a turn that was not saved");

                d.key(SDLK_I);
                d.settle("bars");
                d.check(!d.state().bars, "I did not hide the bars");
                d.key(SDLK_I);
                d.settle("bars");

                d.key(SDLK_H, SDL_KMOD_CTRL);
                d.settle("help");
                d.check(d.state().help, "Ctrl+H did not show the keybinds");

                if (d.state().help) {
                    d.key(SDLK_ESCAPE);
                    d.settle("help");
                    d.check(!d.state().help, "Escape did not put the keybinds away");
                }

                d.key(SDLK_H, SDL_KMOD_CTRL);
                d.settle("help");
                d.click(2.0F, 2.0F);
                d.settle("help");
                d.check(!d.state().help, "a click outside the keybinds did not put them away");

                d.key(SDLK_F);
                d.settle("fullscreen");
                d.check(d.state().fullscreen, "F did not go fullscreen");
                d.key(SDLK_F11);
                d.settle("fullscreen");

                const SDL_Point window = d.size();

                d.click(static_cast<float>(window.x) / 2.0F, static_cast<float>(window.y) / 2.0F, 2);
                d.settle("double click");
                d.check(d.state().fullscreen, "a double click did not go fullscreen");
                d.key(SDLK_F);
                d.settle("fullscreen");

                d.key(SDLK_R, SDL_KMOD_SHIFT);
                d.settle("rebuild in memory");
            });
        }

        void formats(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("formats") / "cache"});

            driver.session(library.any("formats"), [&](Driver &d) {
                open(d);
                walk(library, d, "open", [&] {
                    d.key(SDLK_2);
                    d.settle("actual size");
                    d.key(SDLK_1);
                    d.settle("fit");

                    if (d.state().info.kind != tiv::Decode::Format::Bmp) {
                        return;
                    }

                    d.key(SDLK_RIGHTBRACKET);
                    d.key(SDLK_S, SDL_KMOD_CTRL);
                    d.settle("save");
                    d.check(d.state().message.contains("keep no orientation"), "saving a turned BMP did not refuse");
                    d.key(SDLK_R);
                    d.settle("reload");
                });
            });
        }

        void broken(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("broken") / "cache"});

            driver.session(library.any("broken"), [&](Driver &d) {
                open(d);
                walk(library, d, "open");
            });
        }

        void animated(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("animated") / "cache"});

            driver.session(library.any("animated"), [&](Driver &d) {
                open(d);
                d.key(SDLK_I);
                d.settle("bars");
                walk(library, d, "open", [&] {
                    const tiv::App::State start = d.state();
                    const Entry *entry = library.find(start.file);

                    if (!start.animated || entry == nullptr) {
                        return;
                    }

                    const auto frames = static_cast<std::size_t>(entry->frames);
                    const std::string name = entry->file.filename().string();

                    d.check(start.playing, name + " did not play on its own");

                    // Sampled along the way, as the frames of one loop can add up to the time waited.
                    bool moved = false;

                    for (int waited = 0; waited < PLAY_MS && !moved; waited += PLAY_SAMPLE_MS) {
                        Driver::pause(PLAY_SAMPLE_MS);
                        moved = d.state().frame != start.frame;
                    }

                    d.check(moved, name + " stood still while playing");

                    d.key(SDLK_SPACE);
                    d.settle("pause");
                    d.check(!d.state().playing, "Space did not pause " + name);

                    const std::size_t at = d.state().frame;

                    d.key(SDLK_PERIOD);
                    d.settle("frame");
                    d.check(d.state().frame == (at + 1) % frames, ". did not step a frame on");
                    d.key(SDLK_COMMA);
                    d.settle("frame");
                    d.check(d.state().frame == at, ", did not step a frame back");

                    const SDL_Point window = d.size();
                    const float y = static_cast<float>(window.y) - PLAY_BAR_MIDDLE;

                    d.click(PLAY_BAR_MIDDLE, y);
                    d.settle("play bar");
                    d.check(d.state().playing, "the play mark did not play " + name);
                    d.drag(SDL_BUTTON_LEFT, PLAY_BAR_MIDDLE * 4.0F, y, static_cast<float>(window.x) / 2.0F, 0.0F, 8);
                    d.settle("seek");
                    d.key(SDLK_SPACE);
                    d.settle("pause");
                });
            });
        }

        void vector(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("vector") / "cache"});

            driver.session(library.any("vector"), [&](Driver &d) {
                open(d);
                walk(library, d, "open", [&] {
                    d.key(SDLK_2);
                    d.settle("actual size");

                    for (int i = 0; i < 3; ++i) {
                        d.key(SDLK_UP);
                        d.settle("render again");
                    }

                    look_around(d);
                });
            });
        }

        void oriented(const Library &library, Driver &driver) {
            const std::filesystem::path work = Sandbox::fresh("oriented");

            Sandbox::copy(Sandbox::corpus() / "oriented", work / "oriented");
            configure({.cache = work / "cache"});

            driver.session(in_copy(library, work / "oriented", "oriented"), [&](Driver &d) {
                open(d);
                walk(library, d, "open", [&] {
                    const std::filesystem::path file = d.state().file;
                    const Entry *entry = library.find(file);

                    if (entry == nullptr) {
                        return;
                    }

                    const std::string name = file.filename().string();

                    // Turn right and flip, then save. Undo both in reverse order and save again.
                    d.key(SDLK_RIGHTBRACKET);
                    d.key(SDLK_SEMICOLON);
                    d.settle("turn");
                    d.key(SDLK_S, SDL_KMOD_CTRL);
                    d.settle("save");

                    const tiv::App::State saved = d.state();

                    d.check(saved.message == "Saved", std::format("{} was not saved: {}", name, saved.message));
                    d.check(saved.info.orientation != entry->orientation, name + " kept its orientation tag");

                    d.key(SDLK_SEMICOLON);
                    d.key(SDLK_LEFTBRACKET);
                    d.settle("turn back");
                    d.key(SDLK_S, SDL_KMOD_CTRL);
                    d.settle("save");
                    d.check(slurp(file) == slurp(Sandbox::corpus() / entry->file),
                            name + " is not the same file after turning back and saving");
                    d.key(SDLK_R);
                    d.settle("reload");
                    shown(library, d);
                });
            });
        }

        void streaming(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("streaming") / "cache", .streaming = true});

            driver.session(library.any("browse"), [&](Driver &d) {
                open(d);
                walk(library, d, "stream");
                look_around(d);
                d.key(SDLK_S);
                d.settle("streaming off");
                d.check(!d.state().streaming, "S did not switch streaming off");
                d.key(SDLK_S);
                d.settle("streaming on");
                d.check(d.state().streaming, "S did not switch streaming on");
            });

            driver.session(library.any("huge"), [&](Driver &d) {
                open(d);
                hurry(d);
                walk(library, d, "stream", [&] { look_around(d); });
            });
        }

        // Builds the pyramids on disk, then opens them from disk in a new session.
        void on_disk(const Library &library, Driver &driver, const std::string_view name,
                     const std::string_view cacheMode) {
            const std::filesystem::path cache = Sandbox::fresh(name) / "cache";

            configure({.cache = cache, .cacheMode = cacheMode, .streaming = true, .persist = true});

            driver.session(library.any("huge"), [&](Driver &d) {
                open(d);
                hurry(d);
                walk(library, d, "build", [&] { look_around(d); });
            });

            driver.check(files_under(cache) > 0, "no pyramid was written to disk");

            driver.session(library.any("huge"), [&](Driver &d) {
                open(d);
                walk(library, d, "from disk", [&] { look_around(d); });
                d.key(SDLK_R, SDL_KMOD_SHIFT);
                d.settle("rebuild");
                shown(library, d);
            });
        }

        void persist(const Library &library, Driver &driver) {
            on_disk(library, driver, "persist", "fast");
        }

        void small(const Library &library, Driver &driver) {
            on_disk(library, driver, "small", "small");
        }

        void footprint(const Library &library, Driver &driver) {
            configure({.cache = Sandbox::fresh("footprint") / "cache", .footprint = true});

            driver.session(library.any("browse"), [&](Driver &d) {
                open(d);
                walk(library, d, "step");
                d.key(SDLK_S);
                d.settle("streaming on");
                walk(library, d, "step");
            });

            driver.session(library.any("huge"), [&](Driver &d) {
                open(d);
                walk(library, d, "stream", [&] { look_around(d); });
            });
        }

        // With an empty cache setting, each pyramid goes in a tinyiv-cache folder beside the image.
        void beside(const Library &library, Driver &driver) {
            const std::filesystem::path work = Sandbox::fresh("beside");

            Sandbox::copy(Sandbox::corpus() / "huge", work / "huge");
            configure({.cache = {}, .streaming = true, .persist = true});

            driver.session(in_copy(library, work / "huge", "huge"), [&](Driver &d) {
                open(d);
                walk(library, d, "build");
            });

            driver.check(files_under(work / "huge" / "tinyiv-cache") > 0, "no tinyiv-cache folder beside the images");
        }

        constexpr std::array SITTINGS{
                Sitting{
                        .name = "first-run",
                        .about = "no config, an empty window and a drop, then every setting changed",
                        .run = first_run,
                },
                Sitting{
                        .name = "browse",
                        .about = "every image in turn, wheel flicks, every order, zoom, pan, turns, bars, keybinds "
                                 "and fullscreen",
                        .run = browse,
                },
                Sitting{
                        .name = "formats",
                        .about = "one file of every format the decoders tell apart, at fit and actual size",
                        .run = formats,
                },
                Sitting{.name = "broken", .about = "files that have to fail to open", .run = broken},
                Sitting{
                        .name = "animated",
                        .about = "animations played, paused, stepped and dragged along the play bar",
                        .run = animated,
                },
                Sitting{.name = "vector", .about = "SVG and PDF rendered again as the view zooms", .run = vector},
                Sitting{
                        .name = "oriented",
                        .about = "every orientation shown, turned, saved and turned back to the same file",
                        .run = oriented,
                },
                Sitting{
                        .name = "streaming",
                        .about = "streaming mode with the pyramids in memory, S switched both ways",
                        .run = streaming,
                },
                Sitting{
                        .name = "persist",
                        .about = "huge images built on disk, then opened from there and rebuilt",
                        .run = persist,
                },
                Sitting{
                        .name = "small",
                        .about = "huge images built with cache_mode small, which keeps PNGs as inflate checkpoints",
                        .run = small,
                },
                Sitting{
                        .name = "footprint",
                        .about = "memory_mode footprint over the photos and the huge images",
                        .run = footprint,
                },
                Sitting{
                        .name = "beside",
                        .about = "an empty cache setting, pyramids in tinyiv-cache beside the images",
                        .run = beside,
                },
        };
    }

    const Entry *Library::find(const std::filesystem::path &file) const {
        const auto found = std::ranges::find_if(
                _entries, [&](const Entry &entry) { return entry.file.filename() == file.filename(); });

        return found != _entries.end() ? &*found : nullptr;
    }

    std::vector<const Entry *> Library::in(const std::string_view folder) const {
        std::vector<const Entry *> found;

        for (const Entry &entry : _entries) {
            if (entry.file.parent_path() == folder) {
                found.push_back(&entry);
            }
        }

        return found;
    }

    std::filesystem::path Library::any(const std::string_view folder) const {
        const std::vector<const Entry *> found = in(folder);

        return found.empty() ? std::filesystem::path() : Sandbox::corpus() / found.front()->file;
    }

    std::span<const Sitting> sittings() {
        return SITTINGS;
    }
}
