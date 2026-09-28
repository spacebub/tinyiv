// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"
#include "services/Refiner.h"

namespace tiv {
    Refiner::Refiner(const std::uint32_t eventType) : _event(eventType), _worker([this] { work(); }) {
    }

    Refiner::~Refiner() {
        {
            const std::scoped_lock hold(_guard);

            _running = false;

            if (_abort != nullptr) {
                _abort->request();
            }
        }

        _wake.notify_all();
        _worker.join();
    }

    void Refiner::render(const std::uint64_t generation, const std::filesystem::path &file, const double scale,
                         const int x, const int y, const int width, const int height) {
        {
            const std::scoped_lock hold(_guard);

            _request = Request{
                    .generation = generation,
                    .file = file,
                    .scale = scale,
                    .x = x,
                    .y = y,
                    .width = width,
                    .height = height,
            };

            if (_abort != nullptr) {
                _abort->request();
            }
        }

        _wake.notify_all();
    }

    void Refiner::cancel() {
        const std::scoped_lock hold(_guard);

        _request.reset();
        _result.reset();

        if (_abort != nullptr) {
            _abort->request();
        }
    }

    bool Refiner::take(Result *out) {
        const std::scoped_lock hold(_guard);

        if (!_result.has_value()) {
            return false;
        }

        *out = std::move(*_result);
        _result.reset();

        return true;
    }

    void Refiner::work() {
        std::unique_lock hold(_guard);

        while (_running) {
            if (!_request.has_value()) {
                _wake.wait(hold);

                continue;
            }

            const Request request = std::move(*_request);
            auto abort = std::make_shared<Decode::Abort>();

            _request.reset();
            _abort = abort;
            hold.unlock();

            Bitmap bitmap;
            const bool rendered = Decode::render(request.file, request.scale, request.x, request.y, request.width,
                                                 request.height, &bitmap, nullptr, abort.get());
            auto shared = rendered ? std::make_shared<const Bitmap>(std::move(bitmap)) : nullptr;

            hold.lock();
            _abort = nullptr;

            if (shared == nullptr || abort->requested()) {
                continue;
            }

            _result = Result{
                    .generation = request.generation,
                    .scale = request.scale,
                    .x = request.x,
                    .y = request.y,
                    .bitmap = std::move(shared),
            };

            SDL_Event event{};

            event.type = _event;

            SDL_PushEvent(&event);
        }
    }
}
