// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <cstddef>
#include <cstdint>
#include <span>

#include "render/gpu/Api.h"
#include "render/gpu/Buffer.h"
#include "render/gpu/Device.h"
#include "render/gpu/Staging.h"

namespace tiv::gpu {
namespace {
// Above the copy offset alignment any device asks for.
constexpr std::size_t ALIGNMENT = 256;
}

Staging::Staging(Device &device) : _device(&device), _ring(device, SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) {
}

VkCommandBuffer Staging::commands() {
    if (_open == VK_NULL_HANDLE) {
        _open = _device->begin();
        _openBegin = _head;
        _openUsed = false;
    }

    return _open;
}

void Staging::retire() {
    const std::uint64_t done = _device->completed();

    while (!_segments.empty() && _segments.front().serial <= done) {
        _segments.pop_front();
    }
}

// The bytes in use run from the oldest segment to the head, wrapping past the end. A batch never
// wraps: it is submitted first, so each segment is one plain range.
std::span<std::uint8_t> Staging::reserve(const std::size_t bytes, VkDeviceSize *offset) {
    const std::size_t size = (bytes + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;

    if (size > SIZE || !_ring.valid() || _device->lost()) {
        return {};
    }

    for (;;) {
        retire();

        const bool busy = !_segments.empty() || _openUsed;

        if (!busy) {
            _head = 0;
            _openBegin = 0;
        }

        const std::size_t tail = !_segments.empty() ? _segments.front().begin : _openBegin;
        const bool wrapped = busy && _head <= tail;
        const std::size_t limit = wrapped ? tail : SIZE;

        if (_head + size <= limit) {
            const std::size_t at = _head;

            static_cast<void>(commands());
            _openUsed = true;
            _head += size;
            *offset = at;

            return _ring.bytes().subspan(at, bytes);
        }

        if (!wrapped) {
            submit();
            _head = 0;

            continue;
        }

        submit();

        if (_segments.empty()) {
            continue;
        }

        _device->wait(_segments.front().serial);

        if (_device->lost()) {
            return {};
        }
    }
}

void Staging::submit() {
    if (_open == VK_NULL_HANDLE) {
        return;
    }

    const std::uint64_t serial = _device->submit(_open);

    if (_openUsed) {
        _segments.push_back({.begin = _openBegin, .serial = serial});
    }

    _open = VK_NULL_HANDLE;
    _openUsed = false;
    _openBegin = _head;
}
}
