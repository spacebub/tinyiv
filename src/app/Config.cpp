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
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "app/Config.h"

namespace tiv {
    namespace {
        constexpr std::string_view BLANKS = " \t";

        constexpr std::string_view TEMPLATE =
                "# tinyiv settings: one key = value a line. A line starting with # is a comment.\n"
                "\n"
                "# Where the tiles of images too large for memory are kept. Empty keeps them in a\n"
                "# tinyiv-cache folder beside each image. A relative path is taken from the image's\n"
                "# folder, and ~ stands for the home folder.\n"
                "cache =\n"
                "\n"
                "# fast or small. Fast keeps every tile of a huge PNG, so it pans at once. Small keeps only\n"
                "# places to begin decoding it again, about a third of the room, but panning at full\n"
                "# resolution waits a few tens of milliseconds for each band of rows.\n"
                "cache_mode = fast\n";

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
            // False when the value is not one the key takes.
            bool (*apply)(Config &config, std::string_view value);
        };

        // Scanned in order for each line, which stays nanoseconds at any count a person edits by hand.
        constexpr std::array SETTINGS{
                Setting{.key = "cache",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.cache = expanded(value);

                                    return true;
                                }},
                Setting{.key = "cache_mode",
                        .apply =
                                [](Config &config, const std::string_view value) {
                                    config.small = value == "small";

                                    return value == "small" || value == "fast";
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

        void write_template(const std::filesystem::path &file) {
            std::error_code failure;

            std::filesystem::create_directories(file.parent_path(), failure);

            if (failure) {
                return;
            }

            std::ofstream out(file, std::ios::binary);

            out.write(TEMPLATE.data(), static_cast<std::streamsize>(TEMPLATE.size()));
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
                write_template(file);
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

            if (setting == nullptr) {
                warn(number, std::format("unknown key \"{}\"", key));

                continue;
            }

            if (!setting->apply(config, value)) {
                warn(number, std::format("\"{}\" is not a value {} takes", value, key));
            }
        }

        return config;
    }
}
