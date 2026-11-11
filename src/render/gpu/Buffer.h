// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_BUFFER_H
#define TIV_RENDER_GPU_BUFFER_H


#include <cstddef>
#include <cstdint>
#include <span>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
// Host visible and coherent, mapped for its whole life.
class Buffer {

public:
    Buffer() = default;
    Buffer(Device &device, std::size_t size, VkBufferUsageFlags usage);
    ~Buffer();

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    [[nodiscard]] bool valid() const { return _bytes != nullptr; }
    [[nodiscard]] VkBuffer handle() const { return _buffer; }
    [[nodiscard]] std::size_t size() const { return _size; }
    [[nodiscard]] std::span<std::uint8_t> bytes() const { return {_bytes, _size}; }

private:
    void release();

    Device *_device = nullptr;
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    std::uint8_t *_bytes = nullptr;
    std::size_t _size = 0;
};
}


#endif //TIV_RENDER_GPU_BUFFER_H
