// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cmath>

#include <SDL3/SDL.h>

#include "app/Input.h"
#include "view/Viewport.h"

namespace tiv {
    Input::Action Input::handle(const SDL_Event &event, const float density, Viewport &viewport) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return Action::Quit;

            case SDL_EVENT_KEY_DOWN:
                return key(event.key, viewport);

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const double x = event.button.x * density;
                const double y = event.button.y * density;

                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (event.button.clicks == 2) {
                        _drag = Drag::None;

                        return Action::ToggleFullscreen;
                    }

                    _drag = Drag::Pan;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    _drag = Drag::Zoom;
                    _pressY = y;

                    viewport.begin_zoom(x, y);
                }

                return Action::None;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP:
                _drag = Drag::None;

                return Action::None;

            case SDL_EVENT_MOUSE_MOTION:
                if (_drag == Drag::Pan) {
                    viewport.pan(event.motion.xrel * density, event.motion.yrel * density);

                    return Action::Redraw;
                }

                if (_drag == Drag::Zoom) {
                    const double dy = (event.motion.y * density) - _pressY;

                    viewport.zoom_by(std::exp(-dy * ZOOM_PER_PIXEL));

                    return Action::Redraw;
                }

                return Action::None;

            case SDL_EVENT_MOUSE_WHEEL: {
                // Down is next. SDL reports wheel up as positive, and sums fine wheels into whole notches.
                const int notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? event.wheel.integer_y : -event.wheel.integer_y;

                _wheel += notches;

                return _wheel != 0 ? Action::Scroll : Action::None;
            }

            default:
                return Action::None;
        }
    }

    Input::Action Input::key(const SDL_KeyboardEvent &event, Viewport &viewport) {
        switch (event.key) {
            case SDLK_ESCAPE:
            case SDLK_Q:
                return Action::Quit;
            case SDLK_D:
                return (event.mod & SDL_KMOD_CTRL) != 0 ? Action::Quit : Action::None;
            case SDLK_UP:
                viewport.zoom_centred(ZOOM_STEP);

                return Action::Redraw;
            case SDLK_DOWN:
                viewport.zoom_centred(1.0 / ZOOM_STEP);

                return Action::Redraw;
            case SDLK_1:
                viewport.fit();

                return Action::Redraw;
            case SDLK_2:
                viewport.zoom_to(1.0);

                return Action::Redraw;
            case SDLK_F:
            case SDLK_F11:
                return Action::ToggleFullscreen;
            case SDLK_LEFT:
                --_wheel;

                return Action::Scroll;
            case SDLK_RIGHT:
                ++_wheel;

                return Action::Scroll;
            case SDLK_R:
                return Action::Reload;
            case SDLK_HOME:
                return Action::First;
            case SDLK_END:
                return Action::Last;
            case SDLK_LEFTBRACKET:
                return Action::TurnLeft;
            case SDLK_RIGHTBRACKET:
                return Action::TurnRight;
            case SDLK_SEMICOLON:
                return Action::FlipVertical;
            case SDLK_APOSTROPHE:
                return Action::FlipHorizontal;
            case SDLK_S:
                return (event.mod & SDL_KMOD_CTRL) != 0 ? Action::Save : Action::None;
            case SDLK_H:
                return (event.mod & SDL_KMOD_CTRL) != 0 ? Action::ToggleHelp : Action::None;
            case SDLK_SPACE:
                return Action::TogglePlay;
            case SDLK_COMMA:
                return Action::FrameBack;
            case SDLK_PERIOD:
                return Action::FrameForward;
            default:
                return Action::None;
        }
    }

    int Input::scroll() {
        const int whole = _wheel;

        _wheel = 0;

        return whole;
    }
}
