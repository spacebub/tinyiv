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
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "app/Config.h"
#include "gallery/Folder.h"

namespace tiv {
    namespace {
        constexpr std::string_view BLANKS = " \t";

        std::filesystem::path from_utf8(const std::string_view text) {
            return std::u8string(text.begin(), text.end());
        }

        std::filesystem::path environment(const char *name) {
            const char *value = SDL_getenv(name);

            return value != nullptr && *value != '\0' ? from_utf8(value) : std::filesystem::path{};
        }

        std::filesystem::path home() {
#ifdef _WIN32
            return environment("USERPROFILE");
#else
            return environment("HOME");
#endif
        }

        std::string_view trimmed(std::string_view text) {
            const std::size_t first = text.find_first_not_of(BLANKS);

            if (first == std::string_view::npos) {
                return {};
            }

            text.remove_prefix(first);
            text.remove_suffix(text.size() - text.find_last_not_of(BLANKS) - 1);

            return text;
        }

        // Quotes are optional, for those who write a path with spaces the way a shell wants it.
        std::string_view unquoted(const std::string_view text) {
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                return text.substr(1, text.size() - 2);
            }

            return text;
        }

        struct OrderName {
            std::string_view name;
            Folder::Order order;
        };

        constexpr std::array ORDER_NAMES{
                OrderName{.name = "a-z", .order = Folder::Order::AToZ},
                OrderName{.name = "z-a", .order = Folder::Order::ZToA},
                OrderName{.name = "newest", .order = Folder::Order::Newest},
                OrderName{.name = "oldest", .order = Folder::Order::Oldest},
                OrderName{.name = "size", .order = Folder::Order::Smallest},
        };

        std::filesystem::path expanded(const std::string_view text) {
            if (text == "~") {
                return home();
            }

            if (text.starts_with("~/") || text.starts_with("~\\")) {
                return home() / from_utf8(text.substr(2));
            }

            return from_utf8(text);
        }

        struct Setting {
            std::string_view key;
            // What the file says of it and its default, as written into a file that lacks it.
            std::string_view text;
            // False when the value is not one the key takes.
            bool (*apply)(Config &config, std::string_view value);
        };

        // Scanned in order for each line, which stays nanoseconds at any count a person edits by hand.
        constexpr std::array SETTINGS{
                Setting{.key = "cache",
                        .text = "# Folder for the tile caches of images too large to fit in memory. Leave it empty\n"
                                "# to keep each cache in a tinyiv-cache folder next to its image. A relative path\n"
                                "# starts from the image's folder, and ~ means your home folder.\n"
                                "cache =\n",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.cache = expanded(value);

                                    return true;
                                }},
                Setting{.key = "cache_mode",
                        .text = "# How huge PNGs are cached, fast or small. Fast stores every tile, so panning is\n"
                                "# instant. Small stores only the points decoding can resume from, which takes\n"
                                "# about a third of the space, but panning at full size waits a few tens of\n"
                                "# milliseconds for each band of rows. A cache made before is used as it is, in\n"
                                "# either mode. Shift+R rebuilds the cache of the image on screen in this mode.\n"
                                "cache_mode = fast\n",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.small = value == "small";

                                    return value == "small" || value == "fast";
                                }},
                Setting{.key = "streaming_mode",
                        .text = "# Whether streaming mode is on when tinyiv starts, on or off. S switches it while\n"
                                "# tinyiv runs. Images too large for memory always stream.\n"
                                "streaming_mode = off\n",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.streaming = value == "on";

                                    return value == "on" || value == "off";
                                }},
                Setting{.key = "persist_streams",
                        .text = "# Whether every streamed image keeps its tiles on disk, on or off. Off keeps the\n"
                                "# tiles of an image that fits in memory in memory, so only images too large for\n"
                                "# memory are cached. On caches them all, so they open at once the next time.\n"
                                "persist_streams = off\n",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.persist = value == "on";

                                    return value == "on" || value == "off";
                                }},
                Setting{.key = "sort",
                        .text = "# The order of the images in a folder: a-z, z-a, newest, oldest or size. Newest\n"
                                "# and oldest go by when each file was last modified, and size puts the smallest\n"
                                "# first. O switches it while tinyiv runs, without changing this file.\n"
                                "sort = a-z\n",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    const auto found = std::ranges::find(ORDER_NAMES, value, &OrderName::name);

                                    if (found == ORDER_NAMES.end()) {
                                        return false;
                                    }

                                    config.order = found->order;

                                    return true;
                                }},
        };

        const Setting *setting_for(const std::string_view key) {
            for (const Setting &setting : SETTINGS) {
                if (setting.key == key) {
                    return &setting;
                }
            }

            return nullptr;
        }

        // Every setting the file lacks goes at its end with its default, so all of them can be found
        // there. A file that is not there yet is made with all of them.
        void append_missing(const std::filesystem::path &file, const std::string_view text,
                            const std::span<const bool> seen) {
            if (std::ranges::all_of(seen, [](const bool found) { return found; })) {
                return;
            }

            std::error_code failure;

            std::filesystem::create_directories(file.parent_path(), failure);

            std::ofstream out(file, std::ios::binary | std::ios::app);
            bool first = text.empty();

            if (!first && !text.ends_with('\n')) {
                out << '\n';
            }

            for (std::size_t i = 0; i < SETTINGS.size(); ++i) {
                if (!seen[i]) {
                    out << (first ? "" : "\n") << SETTINGS.at(i).text;
                    first = false;
                }
            }
        }
    }

    std::filesystem::path Config::location() {
#ifdef _WIN32
        // Beside the executable, which the installer puts in a per user folder that can be written.
        const char *exe = SDL_GetBasePath();

        return exe != nullptr ? from_utf8(exe) / "tinyiv.conf" : std::filesystem::path{};
#else
        std::filesystem::path base = environment("XDG_CONFIG_HOME");

        if (base.empty() && !home().empty()) {
            base = home() / ".config";
        }

        return base.empty() ? std::filesystem::path{} : base / "tinyiv" / "tinyiv.conf";
#endif
    }

    Config Config::load(const std::filesystem::path &file, std::vector<std::string> *warnings) {
        Config config;

        if (file.empty()) {
            return config;
        }

        std::ifstream in(file, std::ios::binary | std::ios::ate);

        if (!in) {
            std::error_code failure;

            if (!std::filesystem::exists(file, failure) && !failure) {
                append_missing(file, {}, std::array<bool, SETTINGS.size()>{});
            }

            return config;
        }

        // One read of the whole file, as it is opened at every start.
        std::string text(static_cast<std::size_t>(std::max<std::streamoff>(in.tellg(), 0)), '\0');

        in.seekg(0);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));

        const auto warn = [&](const int number, const std::string_view why) {
            if (warnings != nullptr) {
                warnings->push_back(std::format("{}:{}: {}", file.string(), number, why));
            }
        };

        std::string_view rest = text;
        int number = 0;
        std::array<bool, SETTINGS.size()> seen{};

        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            std::string_view line = rest.substr(0, end);

            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            ++number;

            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }

            line = trimmed(line);

            if (line.empty() || line.starts_with('#')) {
                continue;
            }

            const std::size_t equals = line.find('=');

            if (equals == std::string_view::npos) {
                warn(number, std::format("expected key = value, found \"{}\"", line));

                continue;
            }

            const std::string_view key = trimmed(line.substr(0, equals));
            const std::string_view value = unquoted(trimmed(line.substr(equals + 1)));
            const Setting *setting = setting_for(key);

            if (setting != nullptr) {
                seen.at(static_cast<std::size_t>(setting - SETTINGS.data())) = true;
            }

            if (setting == nullptr) {
                warn(number, std::format("unknown key \"{}\"", key));

                continue;
            }

            if (!setting->apply(config, value)) {
                warn(number, std::format("{} cannot be \"{}\"", key, value));
            }
        }

        append_missing(file, text, seen);

        return config;
    }
}
