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
#include <cmath>

#include "render/Painter.h"
#include "render/PlayBar.h"
#include "view/Viewport.h"

namespace tiv {
namespace {
constexpr float HEIGHT = 24.0F;
constexpr float PADDING = 8.0F;
constexpr float TRACK_HEIGHT = 4.0F;
// The mark takes this share of the bar's height.
constexpr float MARK_SHARE = 0.5F;

constexpr Colour BACKGROUND{.r = 24, .g = 24, .b = 24, .a = 216};
constexpr Colour TRACK{.r = 90, .g = 90, .b = 90, .a = 255};
constexpr Colour FILL{.r = 220, .g = 220, .b = 220, .a = 255};

// The square at the left end that holds the mark.
Rect button(const Rect &bar) {
    return {.x = bar.x, .y = bar.y, .width = bar.height, .height = bar.height};
}

Rect track(const Rect &bar, const float scale) {
    const double padding = PADDING * scale;
    const double x = bar.x + bar.height;

    return {.x = x, .y = bar.y, .width = std::max(bar.x + bar.width - padding - x, 1.0), .height = bar.height};
}

void play_mark(Painter &painter, const Rect &box) {
    const auto x = static_cast<float>(box.x);
    const auto y = static_cast<float>(box.y);
    const auto w = static_cast<float>(box.width);
    const auto h = static_cast<float>(box.height);
    const std::array<Point, 3> corners{
            {
                    {.x = x, .y = y},
                    {.x = x + w, .y = y + (h / 2.0F)},
                    {.x = x, .y = y + h},
            },
    };

    painter.triangles(corners, FILL);
}

void pause_mark(Painter &painter, const Rect &box) {
    const double stroke = box.width / 3.0;

    painter.fill({.x = box.x, .y = box.y, .width = stroke, .height = box.height}, FILL);
    painter.fill({.x = box.x + box.width - stroke, .y = box.y, .width = stroke, .height = box.height}, FILL);
}
}

int PlayBar::height(const float scale) {
    return static_cast<int>(std::lround(HEIGHT * scale));
}

void PlayBar::draw(Painter &painter, const Rect &bar, const float scale, const bool playing, const double progress) {
    painter.fill(bar, BACKGROUND);

    const Rect square = button(bar);
    const double side = square.height * MARK_SHARE;
    const Rect mark{
            .x = square.x + ((square.width - side) / 2.0),
            .y = square.y + ((square.height - side) / 2.0),
            .width = side,
            .height = side,
    };

    // The button shows what a click does.
    if (playing) {
        pause_mark(painter, mark);
    } else {
        play_mark(painter, mark);
    }

    const Rect line = track(bar, scale);
    const double thickness = TRACK_HEIGHT * scale;
    const Rect groove{
            .x = line.x,
            .y = line.y + ((line.height - thickness) / 2.0),
            .width = line.width,
            .height = thickness,
    };

    painter.fill(groove, TRACK);
    painter.fill(
            {
                    .x = groove.x,
                    .y = groove.y,
                    .width = groove.width * std::clamp(progress, 0.0, 1.0),
                    .height = groove.height,
            },
            FILL);
}

PlayBar::Part PlayBar::hit(const Rect &bar, const float scale, const double x, const double y) {
    if (y < bar.y || y >= bar.y + bar.height || x < bar.x || x >= bar.x + bar.width) {
        return Part::None;
    }

    return x < track(bar, scale).x ? Part::Toggle : Part::Track;
}

double PlayBar::seek(const Rect &bar, const float scale, const double x) {
    const Rect line = track(bar, scale);

    return std::clamp((x - line.x) / line.width, 0.0, 1.0);
}
}
