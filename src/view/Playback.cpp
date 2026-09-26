// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "image/Animation.h"
#include "image/Pyramid.h"
#include "view/Playback.h"

namespace tiv {
    void Playback::start(std::shared_ptr<const Animation> animation, const std::uint64_t now) {
        _animation = animation != nullptr && !animation->frames.empty() ? std::move(animation) : nullptr;
        _frame = 0;
        _playing = _animation != nullptr;
        _due = now + delay();
    }

    void Playback::stop() {
        _animation = nullptr;
        _frame = 0;
        _playing = false;
    }

    void Playback::toggle(const std::uint64_t now) {
        if (_animation == nullptr) {
            return;
        }

        _playing = !_playing;
        _due = now + delay();
    }

    void Playback::step(const int delta) {
        if (_animation == nullptr) {
            return;
        }

        const auto count = static_cast<long>(frames());

        _playing = false;
        _frame = static_cast<std::size_t>((((static_cast<long>(_frame) + delta) % count) + count) % count);
    }

    void Playback::seek(const double fraction, const std::uint64_t now) {
        if (_animation == nullptr) {
            return;
        }

        const auto target = static_cast<long>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(_animation->duration()));
        long start = 0;

        _frame = 0;

        while (_frame + 1 < frames() && start + _animation->frames.at(_frame).delay <= target) {
            start += _animation->frames.at(_frame).delay;
            ++_frame;
        }

        _due = now + delay();
    }

    bool Playback::advance(const std::uint64_t now) {
        if (!_playing || now < _due) {
            return false;
        }

        // Behind by a whole run or more, as after a stall, it picks up from now instead of racing to catch up.
        if (now - _due >= static_cast<std::uint64_t>(_animation->duration())) {
            _due = now;
        }

        while (now >= _due) {
            _frame = (_frame + 1) % frames();
            _due += delay();
        }

        return true;
    }

    std::size_t Playback::frames() const {
        return _animation == nullptr ? 0 : _animation->frames.size();
    }

    std::shared_ptr<const Pyramid> Playback::pyramid() const {
        return _animation == nullptr ? nullptr : _animation->frames.at(_frame).pyramid;
    }

    double Playback::progress() const {
        if (_animation == nullptr || _animation->duration() <= 0) {
            return 0.0;
        }

        long start = 0;

        for (std::size_t i = 0; i < _frame; ++i) {
            start += _animation->frames.at(i).delay;
        }

        return static_cast<double>(start) / static_cast<double>(_animation->duration());
    }

    std::uint64_t Playback::delay() const {
        return _animation == nullptr ? 0 : static_cast<std::uint64_t>(std::max(_animation->frames.at(_frame).delay, 1));
    }
}
