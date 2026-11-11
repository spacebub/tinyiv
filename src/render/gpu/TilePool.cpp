// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "image/Bitmap.h"
#include "render/Painter.h"
#include "render/gpu/Api.h"
#include "render/gpu/Device.h"
#include "render/gpu/Staging.h"
#include "render/gpu/TilePool.h"

namespace tiv::gpu {
namespace {
constexpr std::uint32_t MAX_SLABS = 512;

int cell_for(const int side) {
    int cell = TilePool::SMALLEST;

    while (cell < side) {
        cell *= 2;
    }

    return cell;
}

std::uint64_t all_of(const std::uint32_t cells) {
    return cells >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << cells) - 1;
}

VkFormat format_of(const Bitmap::Encoding encoding) {
    return encoding == Bitmap::Encoding::Pq ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
}
}

TilePool::TilePool(Device &device, Staging &staging, VkDescriptorSetLayout layout, VkSampler sampler)
    : _device(&device), _staging(&staging), _layout(layout), _sampler(sampler) {
    const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_SLABS};
    const VkDescriptorPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = MAX_SLABS,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
    };

    _device->check(_device->api().CreateDescriptorPool(_device->handle(), &info, nullptr, &_pool));
}

TilePool::~TilePool() {
    _device->api().DeviceWaitIdle(_device->handle());

    for (Slab &slab : _slabs) {
        destroy(slab);
    }

    if (_pool != VK_NULL_HANDLE) {
        _device->api().DestroyDescriptorPool(_device->handle(), _pool, nullptr);
    }
}

bool TilePool::make(Slab *slab, const Bitmap::Encoding encoding, const int cellWidth, const int cellHeight) {
    const Api &vk = _device->api();
    VkDevice device = _device->handle();
    const VkFormat format = format_of(encoding);

    slab->encoding = encoding;
    slab->cellWidth = cellWidth;
    slab->cellHeight = cellHeight;
    slab->layers = cellWidth == SIDE && cellHeight == SIDE ? WHOLE_LAYERS : 1;
    slab->cells = slab->layers * static_cast<std::uint32_t>((SIDE / cellWidth) * (SIDE / cellHeight));

    const VkImageCreateInfo image{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {.width = SIDE, .height = SIDE, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = slab->layers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (!_device->check(vk.CreateImage(device, &image, nullptr, &slab->image))) {
        return false;
    }

    VkMemoryRequirements needs{};
    vk.GetImageMemoryRequirements(device, slab->image, &needs);

    const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = needs.size,
            .memoryTypeIndex = _device->memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    if (allocation.memoryTypeIndex == Device::NO_MEMORY
        || !_device->check(vk.AllocateMemory(device, &allocation, nullptr, &slab->memory))
        || !_device->check(vk.BindImageMemory(device, slab->image, slab->memory, 0))) {
        return false;
    }

    const VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = slab->layers,
    };
    const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = slab->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format = format,
            .subresourceRange = range,
    };

    if (!_device->check(vk.CreateImageView(device, &view, nullptr, &slab->view))) {
        return false;
    }

    const VkDescriptorSetAllocateInfo set{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = _pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &_layout,
    };

    if (!_device->check(vk.AllocateDescriptorSets(device, &set, &slab->set))) {
        return false;
    }

    const VkDescriptorImageInfo bound{
            .sampler = _sampler,
            .imageView = slab->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slab->set,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &bound,
    };

    vk.UpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Every layer gets a layout the descriptor allows, as the view covers layers never written.
    const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slab->image,
            .subresourceRange = range,
    };

    vk.CmdPipelineBarrier(_staging->commands(),
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0,
                          0,
                          nullptr,
                          0,
                          nullptr,
                          1,
                          &barrier);

    return true;
}

void TilePool::destroy(Slab &slab) {
    const Api &vk = _device->api();
    VkDevice device = _device->handle();

    if (slab.set != VK_NULL_HANDLE) {
        vk.FreeDescriptorSets(device, _pool, 1, &slab.set);
    }

    if (slab.view != VK_NULL_HANDLE) {
        vk.DestroyImageView(device, slab.view, nullptr);
    }

    if (slab.image != VK_NULL_HANDLE) {
        vk.DestroyImage(device, slab.image, nullptr);
    }

    if (slab.memory != VK_NULL_HANDLE) {
        vk.FreeMemory(device, slab.memory, nullptr);
    }

    slab = {};
}

Painter::Tile TilePool::acquire(const int width, const int height, const Bitmap::Encoding encoding) {
    if (width <= 0 || height <= 0 || width > SIDE || height > SIDE) {
        return Painter::NO_TILE;
    }

    const int cellWidth = cell_for(width);
    const int cellHeight = cell_for(height);
    auto open = std::ranges::find_if(_slabs, [&](const Slab &slab) {
        return slab.image != VK_NULL_HANDLE && slab.retired == 0 && slab.encoding == encoding
               && slab.cellWidth == cellWidth && slab.cellHeight == cellHeight && slab.used != all_of(slab.cells);
    });

    if (open == _slabs.end()) {
        auto unused = std::ranges::find_if(_slabs, [](const Slab &slab) { return slab.image == VK_NULL_HANDLE; });

        if (unused == _slabs.end()) {
            if (_slabs.size() >= MAX_SLABS) {
                return Painter::NO_TILE;
            }

            _slabs.emplace_back();
            unused = _slabs.end() - 1;
        }

        if (!make(&*unused, encoding, cellWidth, cellHeight)) {
            destroy(*unused);

            return Painter::NO_TILE;
        }

        open = unused;
    }

    const auto index = static_cast<std::uint32_t>(std::countr_one(open->used));
    const auto slab = static_cast<std::uint32_t>(open - _slabs.begin());

    open->used |= std::uint64_t{1} << index;
    open->sizes.at(std::size_t{2} * index) = static_cast<std::uint16_t>(width);
    open->sizes.at((std::size_t{2} * index) + 1) = static_cast<std::uint16_t>(height);

    return (slab * MOST_CELLS) + index + 1;
}

void TilePool::release(const Painter::Tile tile) {
    if (tile == Painter::NO_TILE) {
        return;
    }

    const std::uint32_t index = (tile - 1) % MOST_CELLS;

    _slabs.at((tile - 1) / MOST_CELLS).used &= ~(std::uint64_t{1} << index);
}

TilePool::Place TilePool::place(const Painter::Tile tile) const {
    const std::uint32_t slab = (tile - 1) / MOST_CELLS;
    const std::uint32_t index = (tile - 1) % MOST_CELLS;
    const Slab &held = _slabs.at(slab);
    const auto across = static_cast<std::uint32_t>(SIDE / held.cellWidth);
    const std::uint32_t perLayer = across * static_cast<std::uint32_t>(SIDE / held.cellHeight);
    const std::uint32_t within = index % perLayer;

    return {
            .slab = slab,
            .layer = index / perLayer,
            .x = static_cast<int>(within % across) * held.cellWidth,
            .y = static_cast<int>(within / across) * held.cellHeight,
            .width = held.sizes.at(std::size_t{2} * index),
            .height = held.sizes.at((std::size_t{2} * index) + 1),
    };
}

std::size_t TilePool::resident_bytes() const {
    std::size_t layers = 0;

    for (const Slab &slab : _slabs) {
        layers += slab.image != VK_NULL_HANDLE ? slab.layers : 0;
    }

    return layers * SIDE * SIDE * Bitmap::CHANNELS;
}

void TilePool::trim() {
    const std::uint64_t done = _device->completed();

    for (Slab &slab : _slabs) {
        if (slab.retired != 0 && slab.retired <= done) {
            destroy(slab);
        }
    }

    // An upload into an empty slab may be waiting in the open batch.
    _staging->submit();

    for (const Bitmap::Encoding encoding : {Bitmap::Encoding::Srgb, Bitmap::Encoding::Pq}) {
        // Panning churns whole tiles only, so only their class keeps a spare.
        bool spare = false;

        for (Slab &slab : _slabs) {
            if (slab.image == VK_NULL_HANDLE || slab.retired != 0 || slab.encoding != encoding || slab.used != 0) {
                continue;
            }

            if (!spare && slab.cellWidth == SIDE && slab.cellHeight == SIDE) {
                spare = true;

                continue;
            }

            // Frames in flight may still sample it, and the staging batch was submitted above.
            slab.retired = std::max<std::uint64_t>(_device->submitted(), 1);
        }
    }
}
}
