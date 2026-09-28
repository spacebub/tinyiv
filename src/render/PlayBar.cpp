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

#include <SDL3/SDL.h>

#include "render/PlayBar.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        constexpr float HEIGHT = 24.0F;
        constexpr float PADDING = 8.0F;
        constexpr float TRACK_HEIGHT = 4.0F;
        // The mark takes this share of the bar's height.
        constexpr float MARK_SHARE = 0.5F;

        constexpr SDL_Color BACKGROUND{.r = 24, .g = 24, .b = 24, .a = 216};
        constexpr SDL_Color TRACK{.r = 90, .g = 90, .b = 90, .a = 255};
        constexpr SDL_Color FILL{.r = 220, .g = 220, .b = 220, .a = 255};

        SDL_FRect frect(const Rect &rect) {
            return {
                    .x = static_cast<float>(rect.x),
                    .y = static_cast<float>(rect.y),
                    .w = static_cast<float>(rect.width),
                    .h = static_cast<float>(rect.height),
            };
        }

        // The square at the left end that holds the mark.
        Rect button(const Rect &bar) {
            return {.x = bar.x, .y = bar.y, .width = bar.height, .height = bar.height};
        }

        Rect track(const Rect &bar, const float scale) {
            const double padding = PADDING * scale;
            const double x = bar.x + bar.height;

            return {.x = x, .y = bar.y, .width = std::max(bar.x + bar.width - padding - x, 1.0), .height = bar.height};
        }

        void colour(SDL_Renderer *renderer, const SDL_Color &with) {
            SDL_SetRenderDrawColor(renderer, with.r, with.g, with.b, with.a);
        }

        void play_mark(SDL_Renderer *renderer, const SDL_FRect &box) {
            const SDL_FColor white{.r = FILL.r / 255.0F, .g = FILL.g / 255.0F, .b = FILL.b / 255.0F, .a = 1.0F};
            const std::array<SDL_Vertex, 3> corners{
                    {
                            {.position = {.x = box.x, .y = box.y}, .color = white, .tex_coord = {}},
                            {
                                    .position = {.x = box.x + box.w, .y = box.y + (box.h / 2.0F)},
                                    .color = white,
                                    .tex_coord = {},
                            },
                            {.position = {.x = box.x, .y = box.y + box.h}, .color = white, .tex_coord = {}},
                    },
            };

            SDL_RenderGeometry(renderer, nullptr, corners.data(), static_cast<int>(corners.size()), nullptr, 0);
        }

        void pause_mark(SDL_Renderer *renderer, const SDL_FRect &box) {
            const float stroke = box.w / 3.0F;
            const std::array<SDL_FRect, 2> bars{
                    {
                            {.x = box.x, .y = box.y, .w = stroke, .h = box.h},
                            {.x = box.x + box.w - stroke, .y = box.y, .w = stroke, .h = box.h},
                    },
            };

            colour(renderer, FILL);
            SDL_RenderFillRects(renderer, bars.data(), static_cast<int>(bars.size()));
        }
    }

    int PlayBar::height(const float scale) {
        return static_cast<int>(std::lround(HEIGHT * scale));
    }

    void PlayBar::draw(SDL_Renderer *renderer, const Rect &bar, const float scale, const bool playing,
                       const double progress) {
        const SDL_FRect area = frect(bar);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        colour(renderer, BACKGROUND);
        SDL_RenderFillRect(renderer, &area);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        const SDL_FRect square = frect(button(bar));
        const float side = square.h * MARK_SHARE;
        const SDL_FRect mark{
                .x = square.x + ((square.w - side) / 2.0F),
                .y = square.y + ((square.h - side) / 2.0F),
                .w = side,
                .h = side,
        };

        // The button shows what a click does.
        if (playing) {
            pause_mark(renderer, mark);
        } else {
            play_mark(renderer, mark);
        }

        const SDL_FRect line = frect(track(bar, scale));
        const float thickness = TRACK_HEIGHT * scale;
        const SDL_FRect groove{.x = line.x, .y = line.y + ((line.h - thickness) / 2.0F), .w = line.w, .h = thickness};
        const SDL_FRect filled{
                .x = groove.x,
                .y = groove.y,
                .w = groove.w * static_cast<float>(std::clamp(progress, 0.0, 1.0)),
                .h = groove.h,
        };

        colour(renderer, TRACK);
        SDL_RenderFillRect(renderer, &groove);
        colour(renderer, FILL);
        SDL_RenderFillRect(renderer, &filled);
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
