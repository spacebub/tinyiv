// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_PIPELINES_H
#define TIV_RENDER_GPU_PIPELINES_H


#include <array>
#include <cstdint>
#include <vector>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
struct TileInstance {
    std::array<float, 4> target{};
    std::array<float, 8> corners{};
    // The texels of the layer that the tile fills: left, top, right, bottom.
    std::array<float, 4> bounds{};
    float layer = 0.0F;
};

struct UiVertex {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    std::uint32_t colour = 0;
};

struct Push {
    float width = 0.0F;
    float height = 0.0F;
    float scale = 1.0F;
    float unused = 0.0F;
};

class Pipelines {

public:
    struct Kit {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageLayout last = VK_IMAGE_LAYOUT_UNDEFINED;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkPipeline tile = VK_NULL_HANDLE;
        VkPipeline ui = VK_NULL_HANDLE;
    };

    explicit Pipelines(Device &device);
    ~Pipelines();

    Pipelines(const Pipelines &) = delete;
    Pipelines(Pipelines &&) = delete;
    Pipelines &operator=(const Pipelines &) = delete;
    Pipelines &operator=(Pipelines &&) = delete;

    [[nodiscard]] bool valid() const { return _layout != VK_NULL_HANDLE; }
    [[nodiscard]] VkDescriptorSetLayout set_layout() const { return _setLayout; }
    [[nodiscard]] VkPipelineLayout layout() const { return _layout; }
    [[nodiscard]] VkSampler sampler() const { return _sampler; }

    // Made on first use. Null on failure.
    [[nodiscard]] const Kit *kit(VkFormat format, VkImageLayout last);

private:
    [[nodiscard]] VkPipeline make(VkRenderPass pass, bool tiles) const;

    Device *_device;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkSampler _sampler = VK_NULL_HANDLE;
    std::vector<Kit> _kits;
};
}


#endif //TIV_RENDER_GPU_PIPELINES_H
