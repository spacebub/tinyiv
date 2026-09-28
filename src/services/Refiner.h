// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_SERVICES_REFINER_H
#define TIV_SERVICES_REFINER_H


#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "image/Bitmap.h"
#include "image/decode/Decode.h"

namespace tiv {
    // The thread that renders part of a scalable image at the zoom on screen. Only the latest
    // request matters: a new one aborts the one in flight.
    class Refiner {

    public:
        struct Result {
            std::uint64_t generation = 0;
            double scale = 1.0;
            // Where the bitmap sits in the image at that scale.
            int x = 0;
            int y = 0;
            std::shared_ptr<const Bitmap> bitmap;
        };

        // The view has to be still this long before the part on screen is rendered again.
        static constexpr int REST_MS = 120;

        explicit Refiner(std::uint32_t eventType);
        ~Refiner();

        Refiner(const Refiner &) = delete;
        Refiner(Refiner &&) = delete;
        Refiner &operator=(const Refiner &) = delete;
        Refiner &operator=(Refiner &&) = delete;

        // The part is in pixels of the image at the scale.
        void render(std::uint64_t generation, const std::filesystem::path &file, double scale, int x, int y, int width, int height);
        void cancel();

        // Main thread. False when nothing new has arrived.
        bool take(Result *out);

    private:
        struct Request {
            std::uint64_t generation = 0;
            std::filesystem::path file;
            double scale = 1.0;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
        };

        void work();

        std::uint32_t _event;

        std::mutex _guard;
        std::condition_variable _wake;
        bool _running = true;
        std::optional<Request> _request;
        std::shared_ptr<Decode::Abort> _abort;
        std::optional<Result> _result;

        std::thread _worker;
    };
}


#endif //TIV_SERVICES_REFINER_H
