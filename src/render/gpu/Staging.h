// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_STAGING_H
#define TIV_RENDER_GPU_STAGING_H


#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>

#include "render/gpu/Api.h"
#include "render/gpu/Buffer.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
// Pixels go into this mapped ring, and one command buffer copies them on to the tiles.
class Staging {

public:
    static constexpr std::size_t SIZE = std::size_t{64} * 1024 * 1024;

    explicit Staging(Device &device);

    [[nodiscard]] bool valid() const { return _ring.valid(); }
    [[nodiscard]] VkBuffer buffer() const { return _ring.handle(); }

    // Begun on first use, and submitted by submit().
    [[nodiscard]] VkCommandBuffer commands();

    // Waits for the GPU while the ring is full. Empty when the bytes can never fit.
    [[nodiscard]] std::span<std::uint8_t> reserve(std::size_t bytes, VkDeviceSize *offset);

    void submit();

private:
    struct Segment {
        std::size_t begin = 0;
        std::uint64_t serial = 0;
    };

    void retire();

    Device *_device;
    Buffer _ring;
    std::size_t _head = 0;
    std::deque<Segment> _segments;
    VkCommandBuffer _open = VK_NULL_HANDLE;
    std::size_t _openBegin = 0;
    bool _openUsed = false;
};
}


#endif //TIV_RENDER_GPU_STAGING_H
