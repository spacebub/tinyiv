// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "render/StatusBar.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        constexpr float TEXT_SCALE = 1.5F;
        constexpr float PADDING = 8.0F;
        constexpr float HEIGHT = 26.0F;
        constexpr std::size_t CUT_MARK = 3;

        constexpr SDL_Color BACKGROUND{24, 24, 24, 255};
        constexpr SDL_Color BADGE{24, 24, 24, 216};
        constexpr SDL_Color TEXT{220, 220, 220, 255};
        constexpr SDL_Color SWEEP{220, 220, 220, 255};

        constexpr float SWEEP_HEIGHT = 2.0F;
        // The sweep crosses the bar in this many ticks.
        constexpr int SWEEP_TICKS = 24;
        constexpr std::string_view SPINNER = "|/-\\";

        std::uint64_t tick() {
            return SDL_GetTicks() / static_cast<std::uint64_t>(StatusBar::LOADING_TICK_MS);
        }

        std::string spinning(const std::string &text) {
            return text + "  " + SPINNER.at(tick() % SPINNER.size());
        }

        // A short highlight running left to right along the top of the area, over and over.
        void sweep(SDL_Renderer *renderer, const SDL_FRect &area, const float scale) {
            const float length = area.w / 6.0F;
            const float travel = area.w + length;
            const float at = (static_cast<float>(tick() % SWEEP_TICKS) / SWEEP_TICKS * travel) - length;
            const SDL_FRect line{area.x + std::max(at, 0.0F), area.y, std::min(at + length, area.w) - std::max(at, 0.0F), SWEEP_HEIGHT * scale};

            if (line.w <= 0.0F) {
                return;
            }

            SDL_SetRenderDrawColor(renderer, SWEEP.r, SWEEP.g, SWEEP.b, SWEEP.a);
            SDL_RenderFillRect(renderer, &line);
        }

        float glyph(const float scale) {
            return SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * TEXT_SCALE * scale;
        }

        float badge_width(const std::string &text, const float scale) {
            return (static_cast<float>(text.size()) * glyph(scale)) + (2.0F * PADDING * scale);
        }

        // Cut to the width in glyphs, ending in three dots when something had to go.
        std::string fitted(const std::string &text, const float width, const float scale) {
            const auto room = static_cast<std::size_t>(std::max(width / glyph(scale), 0.0F));

            if (text.size() <= room) {
                return text;
            }

            if (room <= CUT_MARK) {
                return text.substr(0, room);
            }

            return text.substr(0, room - CUT_MARK) + "...";
        }

        void text_at(SDL_Renderer *renderer, const float x, const float y, const float scale, const std::string &text) {
            const float factor = TEXT_SCALE * scale;

            SDL_SetRenderScale(renderer, factor, factor);
            SDL_SetRenderDrawColor(renderer, TEXT.r, TEXT.g, TEXT.b, TEXT.a);
            SDL_RenderDebugText(renderer, x / factor, y / factor, text.c_str());
            SDL_SetRenderScale(renderer, 1.0F, 1.0F);
        }
    }

    int StatusBar::height(const float scale) {
        return static_cast<int>(std::lround(HEIGHT * scale));
    }

    void StatusBar::draw(SDL_Renderer *renderer, const Rect &bar, const float scale, const std::string &left, const std::string &right, const bool loading) {
        const SDL_FRect area{static_cast<float>(bar.x), static_cast<float>(bar.y), static_cast<float>(bar.width), static_cast<float>(bar.height)};

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, BACKGROUND.r, BACKGROUND.g, BACKGROUND.b, BACKGROUND.a);
        SDL_RenderFillRect(renderer, &area);

        if (loading) {
            sweep(renderer, area, scale);
        }

        const float padding = PADDING * scale;
        const float y = area.y + ((area.h - glyph(scale)) / 2.0F);
        const std::string rightText = fitted(loading ? spinning(right) : right, area.w - (2.0F * padding), scale);
        const float rightWidth = static_cast<float>(rightText.size()) * glyph(scale);
        const std::string leftText = fitted(left, area.w - (3.0F * padding) - rightWidth, scale);

        text_at(renderer, area.x + padding, y, scale, leftText);
        text_at(renderer, area.x + area.w - padding - rightWidth, y, scale, rightText);
    }

    void StatusBar::badge(SDL_Renderer *renderer, const double x, const double y, const float scale, const std::string &given, const bool loading) {
        const std::string text = loading ? spinning(given) : given;
        const float padding = PADDING * scale;
        const float width = badge_width(text, scale);
        const float height = HEIGHT * scale;
        const SDL_FRect area{static_cast<float>(x), static_cast<float>(y) - height, width, height};

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, BADGE.r, BADGE.g, BADGE.b, BADGE.a);
        SDL_RenderFillRect(renderer, &area);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        text_at(renderer, area.x + padding, area.y + ((height - glyph(scale)) / 2.0F), scale, text);
    }

    void StatusBar::notice(SDL_Renderer *renderer, const Rect &area, const float scale, const std::string &text) {
        const double x = area.x + ((area.width - badge_width(text, scale)) / 2.0);
        const double y = area.y + ((area.height + (HEIGHT * scale)) / 2.0);

        badge(renderer, std::max(x, area.x), y, scale, text, false);
    }
}
