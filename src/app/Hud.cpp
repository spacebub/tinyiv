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
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <SDL3/SDL.h>

#include "app/Hud.h"
#include "app/Picture.h"
#include "gallery/Folder.h"
#include "render/PlayBar.h"
#include "render/StatusBar.h"
#include "tiv_git_revision.h"
#include "view/Viewport.h"

namespace tiv::Hud {
namespace {
constexpr double BADGE_MARGIN = 12.0;

// Keys are two spaces apart. The words of one key are a single space apart.
constexpr std::array<StatusBar::Row, 19> HELP = {
        {
                {.left = "Wheel  ←  →", .right = "Previous, next image"},
                {.left = "Home  End", .right = "First, last image"},
                {.left = "O", .right = "Sort by name, date, size"},
                {.left = "R", .right = "Reload from disk"},
                {.left = "Shift+R", .right = "Rebuild the tile cache"},
                {.left = "Left drag", .right = "Pan"},
                {.left = "Right drag ↑ ↓", .right = "Zoom in, out"},
                {.left = "↑  ↓", .right = "Zoom in, out a step"},
                {.left = "0  1  2", .right = "Centre, fit, actual size"},
                {.left = "F  F11  Double click", .right = "Fullscreen"},
                {.left = "[  ]", .right = "Turn left, right"},
                {.left = ";  '", .right = "Flip vertically, horizontally"},
                {.left = "Ctrl+S", .right = "Save turns and flips"},
                {.left = "S", .right = "Streaming mode on, off"},
                {.left = "I", .right = "Bars on, off"},
                {.left = "Space", .right = "Play, pause animation"},
                {.left = ",  .", .right = "Previous, next frame"},
                {.left = "Ctrl+H", .right = "Show, hide this help"},
                {.left = "Esc  Q  Ctrl+D", .right = "Quit"},
        },
};

constexpr std::array ORDERS{
        OrderText{.order = Folder::Order::AToZ, .tag = "A-Z", .text = "Sorted by name, A to Z"},
        OrderText{.order = Folder::Order::ZToA, .tag = "Z-A", .text = "Sorted by name, Z to A"},
        OrderText{.order = Folder::Order::Newest, .tag = "NEWEST", .text = "Sorted by date, newest first"},
        OrderText{.order = Folder::Order::Oldest, .tag = "OLDEST", .text = "Sorted by date, oldest first"},
        OrderText{
                .order = Folder::Order::Smallest,
                .tag = "SMALLEST",
                .text = "Sorted by size, smallest first",
        },
};

const OrderText &order_text(const Folder::Order order) {
    return *std::ranges::find(ORDERS, order, &OrderText::order);
}

std::string human_size(const std::uintmax_t bytes) {
    constexpr double KB = 1000.0;
    constexpr double MB = KB * 1000.0;
    constexpr double GB = MB * 1000.0;
    const auto amount = static_cast<double>(bytes);

    if (amount < KB) {
        return std::format("{} B", bytes);
    }

    // Each unit gives way where rounding would show 1000 of it.
    if (amount < MB - (KB / 2.0)) {
        return std::format("{:.0f} kB", amount / KB);
    }

    if (amount < GB - (MB / 20.0)) {
        return std::format("{:.1f} MB", amount / MB);
    }

    return std::format("{:.1f} GB", amount / GB);
}

// The two halves of the title: where in the folder, then the name.
std::string position(const Frame &frame) {
    if (frame.folder->count() == 0) {
        return "";
    }

    return std::format("[{}/{}]", frame.folder->index() + 1, frame.folder->count());
}

// A mark shows while the image is turned and not saved.
std::string name(const Frame &frame) {
    if (frame.folder->count() == 0) {
        return "tinyiv";
    }

    return frame.folder->current().filename().string() + (frame.picture->turned() ? " *" : "");
}

// The place in the folder and the image name.
std::string title(const Frame &frame) {
    const std::string where = position(frame);

    return where.empty() ? name(frame) : where + " " + name(frame);
}

std::string modes(const Frame &frame) {
    std::string text;

    for (const auto &[shown, tag] : {
                 std::pair{frame.streaming, "STREAMING"},
                 std::pair{frame.picture->stale(), "STALE CACHE"},
                 std::pair{frame.linear, "HDR"},
         }) {
        if (shown) {
            text += text.empty() ? tag : std::format("  {}", tag);
        }
    }

    return text;
}

// " in" and then the folder that new tiles go to, with the home folder written as ~.
// Empty when the tiles stay in memory.
std::string where_tiles_go(const Frame &frame) {
    if (frame.picture->tile_folder().empty()) {
        return {};
    }

#ifdef _WIN32
    const char *home = SDL_getenv("USERPROFILE");
#else
    const char *home = SDL_getenv("HOME");
#endif
    const std::string folder = frame.picture->tile_folder().string();

    if (home != nullptr && *home != '\0' && folder.starts_with(home)) {
        return " in ~" + folder.substr(std::string_view(home).size());
    }

    return " in " + folder;
}

std::string details(const Frame &frame) {
    // While there is no image to describe.
    if (frame.folder->count() == 0) {
        if constexpr (*TIV_GIT_REVISION != '\0') {
            return "v" TIV_VERSION " (" TIV_GIT_REVISION ")";
        }

        return "v" TIV_VERSION;
    }

    const Picture &picture = *frame.picture;
    const Decode::Info &info = picture.info();
    const Picture::Size size = picture.shown();
    const std::string format = info.hdr ? info.format + " HDR" : info.format;

    // Switching the mode says so for a moment, as the badge does in fullscreen.
    if (flashing(picture.switched_at())) {
        return picture.flash();
    }

    if (!picture.message().empty()) {
        return picture.message();
    }

    if (info.width == 0) {
        return human_size(picture.bytes());
    }

    if (frame.playback->active()) {
        return std::format("{}x{}, {} {}/{}, {}",
                           size.width,
                           size.height,
                           format,
                           frame.playback->frame() + 1,
                           frame.playback->frames(),
                           human_size(picture.bytes()));
    }

    return std::format("{}x{}, {}, {}{}",
                       size.width,
                       size.height,
                       format,
                       picture.always_streamed() ? "streaming, " : "",
                       human_size(picture.bytes()));
}
}

void draw(SDL_Renderer *renderer, const Frame &frame) {
    const int width = frame.width;
    const int height = frame.height;
    const float scale = frame.scale;
    const int bar = frame.bar;
    const Rect view{
            .x = 0.0,
            .y = static_cast<double>(bar),
            .width = frame.viewport->area_width(),
            .height = frame.viewport->area_height(),
    };

    if (!frame.viewport->has_image()) {
        if (!frame.picture->failure().empty()) {
            StatusBar::notice(renderer, view, scale, frame.picture->failure());
        } else if (frame.picture->building()) {
            const std::string notice =
                    std::format("Making tiles{}: {:.0f}%", where_tiles_go(frame), frame.progress * 100.0F);

            StatusBar::notice(renderer, view, scale, notice);
        } else if (frame.folder->count() == 0) {
            StatusBar::notice(renderer, view, scale, "Drop an image here to open it");
        }
    }

    if (frame.playback->active()) {
        PlayBar::draw(renderer,
                      play_bar(*frame.viewport, scale, bar),
                      scale,
                      frame.playback->playing(),
                      frame.playback->progress());
    }

    if (frame.help) {
        StatusBar::table(
                renderer,
                {.x = 0.0, .y = 0.0, .width = static_cast<double>(width), .height = static_cast<double>(height)},
                scale,
                HELP);
    }

    if (frame.bars) {
        const std::string order = frame.folder->count() > 0 ? std::string(order_text(frame.folder->order()).tag) : "";

        StatusBar::draw(renderer,
                        {.x = 0.0, .y = 0.0, .width = static_cast<double>(width), .height = static_cast<double>(bar)},
                        scale,
                        position(frame),
                        name(frame),
                        order,
                        false);
        StatusBar::draw(renderer,
                        {
                                .x = 0.0,
                                .y = static_cast<double>(height - bar),
                                .width = static_cast<double>(width),
                                .height = static_cast<double>(bar),
                        },
                        scale,
                        modes(frame),
                        "",
                        details(frame),
                        frame.picture->loading());
    } else if (flashing(frame.picture->switched_at())) {
        // No bar to carry the mode, so switching it says so for a moment.
        StatusBar::badge(
                renderer, BADGE_MARGIN * scale, height - (BADGE_MARGIN * scale), scale, frame.picture->flash(), false);
    } else if (frame.picture->loading()) {
        // Where the bar would be, for as long as something is still on its way.
        StatusBar::badge(renderer, BADGE_MARGIN * scale, height - (BADGE_MARGIN * scale), scale, title(frame), true);
    }
}

Rect play_bar(const Viewport &viewport, const float scale, const int bar) {
    const auto height = static_cast<double>(PlayBar::height(scale));

    return {
            .x = 0.0,
            .y = static_cast<double>(bar) + viewport.area_height() - height,
            .width = viewport.area_width(),
            .height = height,
    };
}

Rect help_box(const Rect &area, const float scale) {
    return StatusBar::table_box(area, scale, HELP);
}

bool flashing(const std::uint64_t switchedAt) {
    return switchedAt != 0 && SDL_GetTicks() - switchedAt < MODE_FLASH_MS;
}

// Loader errors lead with the file's path, which the text drops to keep only the reason.
std::string failure_text(const Folder &folder, const std::string &error) {
    const std::string prefix = folder.count() > 0 ? folder.current().string() + ": " : std::string();

    if (prefix.empty() || !error.starts_with(prefix) || error.size() == prefix.size()) {
        return "Could not open this image";
    }

    return "Could not open this image: " + error.substr(prefix.size());
}

const OrderText &next_order(const Folder::Order order) {
    const auto *const at = std::ranges::find(ORDERS, order, &OrderText::order);

    return std::next(at) == ORDERS.end() ? ORDERS.front() : *std::next(at);
}

}
