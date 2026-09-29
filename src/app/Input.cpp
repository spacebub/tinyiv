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
    Input::Action Input::handle(const SDL_Event &event, const float density, const double top, Viewport &viewport) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return Action::Quit;

            case SDL_EVENT_KEY_DOWN:
                return key(event.key, viewport);

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const double x = event.button.x * density;
                const double y = (event.button.y * density) - top;

                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (event.button.clicks == 2) {
                        _drag = Drag::None;

                        return Action::ToggleFullscreen;
                    }

                    _drag = Drag::Pan;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    _drag = Drag::Zoom;
                    _pressZoom = viewport.zoom();
                    _zoomTravel = 0.0;

                    viewport.begin_zoom(x, y);
                }

                return Action::None;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP:
                _drag = Drag::None;
                release(event.button.windowID);

                return Action::None;

            case SDL_EVENT_MOUSE_MOTION:
                if (_drag == Drag::None) {
                    return Action::None;
                }

                follow(event.motion);

                if (_drag == Drag::Pan) {
                    viewport.pan(event.motion.xrel * density, event.motion.yrel * density);
                } else {
                    _zoomTravel += event.motion.yrel * density;
                    viewport.zoom_by(std::exp(-_zoomTravel * ZOOM_PER_PIXEL));
                    // Travel past the zoom limits is dropped, so turning back acts at once.
                    _zoomTravel = -std::log(viewport.zoom() / _pressZoom) / ZOOM_PER_PIXEL;
                }

                return Action::Redraw;

            case SDL_EVENT_MOUSE_WHEEL: {
                // Down is next. SDL reports wheel up as positive, and sums fine wheels into whole notches.
                // SDL_MouseWheelEvent: https://wiki.libsdl.org/SDL3/SDL_MouseWheelEvent
                const int notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? event.wheel.integer_y
                                                                                    : -event.wheel.integer_y;

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
            case SDLK_0:
                viewport.centre();

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
                return (event.mod & SDL_KMOD_CTRL) != 0 ? Action::Save : Action::ToggleStream;
            case SDLK_O:
                return Action::NextOrder;
            case SDLK_I:
                return Action::ToggleBar;
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

    void Input::follow(const SDL_MouseMotionEvent &motion) {
        SDL_Window *window = SDL_GetWindowFromID(motion.windowID);

        // Grabbed on the first motion, so a plain click never confines the cursor.
        if (!_held) {
            SDL_SetWindowMouseGrab(window, true);
            _held = true;
        }

        int width = 0;
        int height = 0;

        SDL_GetWindowSize(window, &width, &height);

        const bool relative = SDL_GetWindowRelativeMouseMode(window);
        // Leaving takes a few units, so a cursor wobbling on an edge does not flip modes on every motion.
        const float margin = relative ? EDGE_RELEASE : 1.0F;
        const bool edge = motion.x < margin || motion.y < margin || motion.x >= static_cast<float>(width) - margin
                          || motion.y >= static_cast<float>(height) - margin;

        // Relative mode reports where the cursor would be, clamped to the window, and SDL warps it
        // there on the way out.
        if (edge != relative) {
            SDL_SetWindowRelativeMouseMode(window, edge);
        }
    }

    void Input::release(const SDL_WindowID window) {
        if (_held) {
            SDL_Window *handle = SDL_GetWindowFromID(window);

            SDL_SetWindowRelativeMouseMode(handle, false);
            SDL_SetWindowMouseGrab(handle, false);
            _held = false;
        }
    }

    int Input::scroll() {
        const int whole = _wheel;

        _wheel = 0;

        return whole;
    }
}
