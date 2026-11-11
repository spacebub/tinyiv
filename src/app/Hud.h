// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_HUD_H
#define TIV_APP_HUD_H


#include <cstdint>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "app/Picture.h"
#include "gallery/Folder.h"
#include "render/Painter.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv::Hud {

// How long fullscreen shows the streaming mode or the order after it switches.
constexpr std::uint64_t MODE_FLASH_MS = 1500;

struct Frame {
    int width = 0;
    int height = 0;
    float scale = 1.0F;
    // The top and the bottom bar share this height. It is zero while they are hidden.
    int bar = 0;
    bool bars = false;
    bool help = false;
    bool hdr10 = false;
    bool streaming = false;
    float progress = 0.0F;
    const Folder *folder = nullptr;
    const Viewport *viewport = nullptr;
    const Playback *playback = nullptr;
    const Picture *picture = nullptr;
};

void draw(Painter &painter, const Frame &frame);

// Along the bottom of the view, over the image.
[[nodiscard]] Rect play_bar(const Viewport &viewport, float scale, int bar);

[[nodiscard]] Rect help_box(const Rect &area, float scale);

// The streaming mode shows for a moment after it switches, in the bar or as a badge.
[[nodiscard]] bool flashing(std::uint64_t switchedAt);

[[nodiscard]] std::string failure_text(const Folder &folder, const std::string &error);

// In the order O steps through them.
struct OrderText {
    Folder::Order order;
    // Ends the top bar for as long as the order holds.
    std::string_view tag;
    // Said for a moment when O switches to it.
    std::string_view text;
};

[[nodiscard]] const OrderText &next_order(Folder::Order order);

}


#endif //TIV_APP_HUD_H
