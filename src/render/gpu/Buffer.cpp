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
#include <utility>

#include "render/gpu/Api.h"
#include "render/gpu/Buffer.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
Buffer::Buffer(Device &device, const std::size_t size, const VkBufferUsageFlags usage) : _device(&device), _size(size) {
    const Api &vk = device.api();
    const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (!device.check(vk.CreateBuffer(device.handle(), &info, nullptr, &_buffer))) {
        return;
    }

    VkMemoryRequirements needs{};
    vk.GetBufferMemoryRequirements(device.handle(), _buffer, &needs);

    // System memory, which the GPU reads over the bus. VRAM is left to the tiles.
    const std::uint32_t type =
            device.memory_type(needs.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = needs.size,
            .memoryTypeIndex = type,
    };
    void *mapped = nullptr;

    if (type == Device::NO_MEMORY || !device.check(vk.AllocateMemory(device.handle(), &allocation, nullptr, &_memory))
        || !device.check(vk.BindBufferMemory(device.handle(), _buffer, _memory, 0))
        || !device.check(vk.MapMemory(device.handle(), _memory, 0, VK_WHOLE_SIZE, 0, &mapped))) {
        release();

        return;
    }

    _bytes = static_cast<std::uint8_t *>(mapped);
}

Buffer::~Buffer() {
    release();
}

Buffer::Buffer(Buffer &&other) noexcept
    : _device(std::exchange(other._device, nullptr)),
      _buffer(std::exchange(other._buffer, VK_NULL_HANDLE)),
      _memory(std::exchange(other._memory, VK_NULL_HANDLE)),
      _bytes(std::exchange(other._bytes, nullptr)),
      _size(std::exchange(other._size, 0)) {
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
    if (this != &other) {
        release();
        _device = std::exchange(other._device, nullptr);
        _buffer = std::exchange(other._buffer, VK_NULL_HANDLE);
        _memory = std::exchange(other._memory, VK_NULL_HANDLE);
        _bytes = std::exchange(other._bytes, nullptr);
        _size = std::exchange(other._size, 0);
    }

    return *this;
}

void Buffer::release() {
    if (_device == nullptr) {
        return;
    }

    const Api &vk = _device->api();

    if (_buffer != VK_NULL_HANDLE) {
        vk.DestroyBuffer(_device->handle(), _buffer, nullptr);
    }

    if (_memory != VK_NULL_HANDLE) {
        vk.FreeMemory(_device->handle(), _memory, nullptr);
    }

    _buffer = VK_NULL_HANDLE;
    _memory = VK_NULL_HANDLE;
    _bytes = nullptr;
    _size = 0;
}
}
