// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_VIEW_PLAYBACK_H
#define TIV_VIEW_PLAYBACK_H


#include <cstddef>
#include <cstdint>
#include <memory>

#include "image/Animation.h"
#include "image/Pyramid.h"

namespace tiv {
    // Times are milliseconds on any steady clock the caller keeps using. It loops for ever.
    class Playback {

    public:
        // Plays from the first frame.
        void start(std::shared_ptr<const Animation> animation, std::uint64_t now);
        void stop();

        [[nodiscard]] bool active() const { return _animation != nullptr; }
        [[nodiscard]] bool playing() const { return _playing; }
        [[nodiscard]] const std::shared_ptr<const Animation> &animation() const { return _animation; }

        void toggle(std::uint64_t now);

        // Pauses and moves by whole frames, wrapping at either end.
        void step(int delta);

        // Jumps to the frame showing at that fraction of the whole run.
        void seek(double fraction, std::uint64_t now);

        // Steps past every frame whose time is up. True when the frame changed.
        bool advance(std::uint64_t now);

        [[nodiscard]] std::size_t frame() const { return _frame; }
        [[nodiscard]] std::size_t frames() const;
        [[nodiscard]] std::shared_ptr<const Pyramid> pyramid() const;

        // When the next frame is due, while playing.
        [[nodiscard]] std::uint64_t due() const { return _due; }

        // Where the current frame starts, as a fraction of the whole run.
        [[nodiscard]] double progress() const;

    private:
        [[nodiscard]] std::uint64_t delay() const;

        std::shared_ptr<const Animation> _animation;
        std::size_t _frame = 0;
        bool _playing = false;
        std::uint64_t _due = 0;
    };
}


#endif //TIV_VIEW_PLAYBACK_H
