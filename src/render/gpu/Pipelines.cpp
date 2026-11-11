// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"
#include "render/gpu/Pipelines.h"
#include "render/gpu/shaders/tile.frag.h"
#include "render/gpu/shaders/tile.vert.h"
#include "render/gpu/shaders/ui.frag.h"
#include "render/gpu/shaders/ui.vert.h"

namespace tiv::gpu {
namespace {
VkShaderModule module_of(const Device &device, const std::span<const std::uint32_t> words) {
    const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words.size_bytes(),
            .pCode = words.data(),
    };
    VkShaderModule made = VK_NULL_HANDLE;

    device.api().CreateShaderModule(device.handle(), &info, nullptr, &made);

    return made;
}
}

Pipelines::Pipelines(Device &device) : _device(&device) {
    const Api &vk = device.api();
    const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding,
    };
    const VkPushConstantRange push{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = sizeof(Push),
    };
    const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0F,
    };

    if (!device.check(vk.CreateDescriptorSetLayout(device.handle(), &setInfo, nullptr, &_setLayout))
        || !device.check(vk.CreateSampler(device.handle(), &sampler, nullptr, &_sampler))) {
        return;
    }

    const VkPipelineLayoutCreateInfo layout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &_setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
    };

    if (!device.check(vk.CreatePipelineLayout(device.handle(), &layout, nullptr, &_layout))) {
        _layout = VK_NULL_HANDLE;
    }
}

Pipelines::~Pipelines() {
    const Api &vk = _device->api();
    VkDevice device = _device->handle();

    for (const Kit &kit : _kits) {
        vk.DestroyPipeline(device, kit.tile, nullptr);
        vk.DestroyPipeline(device, kit.ui, nullptr);
        vk.DestroyRenderPass(device, kit.pass, nullptr);
    }

    vk.DestroyPipelineLayout(device, _layout, nullptr);
    vk.DestroySampler(device, _sampler, nullptr);
    vk.DestroyDescriptorSetLayout(device, _setLayout, nullptr);
}

const Pipelines::Kit *Pipelines::kit(const VkFormat format, const VkImageLayout last) {
    for (const Kit &kit : _kits) {
        if (kit.format == format && kit.last == last) {
            return &kit;
        }
    }

    const VkAttachmentDescription attachment{
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = last,
    };
    const VkAttachmentReference colour{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colour,
    };
    // The submit waits for the acquired image at colour output. The layout change has to wait there too.
    const VkSubpassDependency dependency{
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
    };
    Kit made{.format = format, .last = last};

    if (!_device->check(_device->api().CreateRenderPass(_device->handle(), &info, nullptr, &made.pass))) {
        return nullptr;
    }

    made.tile = make(made.pass, true);
    made.ui = make(made.pass, false);

    if (made.tile == VK_NULL_HANDLE || made.ui == VK_NULL_HANDLE) {
        const Api &vk = _device->api();

        vk.DestroyPipeline(_device->handle(), made.tile, nullptr);
        vk.DestroyPipeline(_device->handle(), made.ui, nullptr);
        vk.DestroyRenderPass(_device->handle(), made.pass, nullptr);

        return nullptr;
    }

    _kits.push_back(made);

    return &_kits.back();
}

VkPipeline Pipelines::make(VkRenderPass pass, const bool tiles) const {
    const Api &vk = _device->api();
    VkShaderModule vertex =
            module_of(*_device, tiles ? std::span<const std::uint32_t>(Spirv::TILE_VERT) : Spirv::UI_VERT);
    VkShaderModule fragment =
            module_of(*_device, tiles ? std::span<const std::uint32_t>(Spirv::TILE_FRAG) : Spirv::UI_FRAG);
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
            {
                    {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            .stage = VK_SHADER_STAGE_VERTEX_BIT,
                            .module = vertex,
                            .pName = "main",
                    },
                    {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                            .module = fragment,
                            .pName = "main",
                    },
            },
    };

    const VkVertexInputBindingDescription tileBinding{
            .binding = 0,
            .stride = sizeof(TileInstance),
            .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    const std::array<VkVertexInputAttributeDescription, 5> tileAttributes{
            {
                    {.location = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(TileInstance, target)},
                    {.location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(TileInstance, corners)},
                    {
                            .location = 2,
                            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = offsetof(TileInstance, corners) + (4 * sizeof(float)),
                    },
                    {.location = 3, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(TileInstance, bounds)},
                    {.location = 4, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(TileInstance, layer)},
            },
    };
    const VkVertexInputBindingDescription uiBinding{
            .binding = 0,
            .stride = sizeof(UiVertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array<VkVertexInputAttributeDescription, 3> uiAttributes{
            {
                    {.location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(UiVertex, x)},
                    {.location = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(UiVertex, u)},
                    {.location = 2, .format = VK_FORMAT_R8G8B8A8_UNORM, .offset = offsetof(UiVertex, colour)},
            },
    };
    const VkPipelineVertexInputStateCreateInfo input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = tiles ? &tileBinding : &uiBinding,
            .vertexAttributeDescriptionCount =
                    static_cast<std::uint32_t>(tiles ? tileAttributes.size() : uiAttributes.size()),
            .pVertexAttributeDescriptions = tiles ? tileAttributes.data() : uiAttributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = tiles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState blend{
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) | VK_COLOR_COMPONENT_G_BIT
                              | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blending{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blend,
    };
    const std::array<VkDynamicState, 2> dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamics.size()),
            .pDynamicStates = dynamics.data(),
    };
    const VkGraphicsPipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &input,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blending,
            .pDynamicState = &dynamic,
            .layout = _layout,
            .renderPass = pass,
            .subpass = 0,
    };
    VkPipeline made = VK_NULL_HANDLE;

    if (vertex != VK_NULL_HANDLE && fragment != VK_NULL_HANDLE) {
        vk.CreateGraphicsPipelines(_device->handle(), VK_NULL_HANDLE, 1, &info, nullptr, &made);
    }

    vk.DestroyShaderModule(_device->handle(), vertex, nullptr);
    vk.DestroyShaderModule(_device->handle(), fragment, nullptr);

    return made;
}
}
