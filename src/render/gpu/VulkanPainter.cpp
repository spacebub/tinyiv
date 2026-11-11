// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Bitmap.h"
#include "image/Orient.h"
#include "image/Tone.h"
#include "render/Font.h"
#include "render/Painter.h"
#include "render/WaylandColor.h"
#include "render/gpu/Api.h"
#include "render/gpu/Buffer.h"
#include "render/gpu/Device.h"
#include "render/gpu/Pipelines.h"
#include "render/gpu/Staging.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/TilePool.h"
#include "render/gpu/VulkanPainter.h"

namespace tiv::gpu {
namespace {
// Glyphs sit in cells with a blank pixel all round, so filtering at the edge reads nothing of a neighbour.
constexpr int CELL = Font::SIZE + 2;
constexpr int ATLAS_COLUMNS = 16;
constexpr int ATLAS_ROWS = (Font::GLYPHS + ATLAS_COLUMNS) / ATLAS_COLUMNS;
constexpr int ATLAS_WIDTH = CELL * ATLAS_COLUMNS;
constexpr int ATLAS_HEIGHT = CELL * ATLAS_ROWS;
// The cell after the last glyph is all lit, for fills.
constexpr int WHITE = Font::GLYPHS;
constexpr std::size_t FIRST_BUFFER = std::size_t{64} * 1024;

std::uint32_t packed(const Colour colour) {
    return static_cast<std::uint32_t>(colour.r) | (static_cast<std::uint32_t>(colour.g) << 8U)
           | (static_cast<std::uint32_t>(colour.b) << 16U) | (static_cast<std::uint32_t>(colour.a) << 24U);
}

float cell_x(const int cell) {
    return static_cast<float>((cell % ATLAS_COLUMNS) * CELL);
}

float cell_y(const int cell) {
    const int row = cell / ATLAS_COLUMNS;

    return static_cast<float>(row * CELL);
}

VkImageMemoryBarrier layer_barrier(VkImage image,
                                   const std::uint32_t layer,
                                   const VkImageLayout from,
                                   const VkImageLayout to,
                                   const VkAccessFlags src,
                                   const VkAccessFlags dst) {
    return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = src,
            .dstAccessMask = dst,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                    },
    };
}

struct Destination {
    VkImage image = VK_NULL_HANDLE;
    std::uint32_t layer = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    // UNDEFINED discards the layer. Any other layout keeps the cells beside this one.
    VkImageLayout from = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// The first layout change waits for earlier frames that sampled the layer.
bool stage(const Device &device,
           Staging &staging,
           const Destination &to,
           const std::uint8_t *pixels,
           const std::size_t pitch) {
    VkImage image = to.image;
    const std::uint32_t layer = to.layer;
    const int width = to.width;
    const int height = to.height;
    const std::size_t row = static_cast<std::size_t>(width) * Bitmap::CHANNELS;
    VkDeviceSize offset = 0;
    const std::span<std::uint8_t> into = staging.reserve(row * static_cast<std::size_t>(height), &offset);

    if (into.empty()) {
        return false;
    }

    if (pitch == row) {
        std::memcpy(into.data(), pixels, into.size());
    } else {
        const std::span<const std::uint8_t> from(pixels, (pitch * static_cast<std::size_t>(height - 1)) + row);

        for (std::size_t y = 0; std::cmp_less(y, height); ++y) {
            std::ranges::copy(from.subspan(y * pitch, row), into.subspan(y * row, row).begin());
        }
    }

    const Api &vk = device.api();
    VkCommandBuffer commands = staging.commands();
    const VkImageMemoryBarrier before =
            layer_barrier(image, layer, to.from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkImageMemoryBarrier after = layer_barrier(image,
                                                     layer,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT,
                                                     VK_ACCESS_SHADER_READ_BIT);
    const VkBufferImageCopy region{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                    },
            .imageOffset = {.x = to.x, .y = to.y, .z = 0},
            .imageExtent =
                    {
                            .width = static_cast<std::uint32_t>(width),
                            .height = static_cast<std::uint32_t>(height),
                            .depth = 1,
                    },
    };

    vk.CmdPipelineBarrier(commands,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0,
                          0,
                          nullptr,
                          0,
                          nullptr,
                          1,
                          &before);
    vk.CmdCopyBufferToImage(commands, staging.buffer(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk.CmdPipelineBarrier(commands,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0,
                          0,
                          nullptr,
                          0,
                          nullptr,
                          1,
                          &after);

    return true;
}

bool make_image(
        Device &device, const VkImageCreateInfo &info, VkImage *image, VkDeviceMemory *memory, VkImageView *view) {
    const Api &vk = device.api();

    if (!device.check(vk.CreateImage(device.handle(), &info, nullptr, image))) {
        return false;
    }

    VkMemoryRequirements needs{};
    vk.GetImageMemoryRequirements(device.handle(), *image, &needs);

    const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = needs.size,
            .memoryTypeIndex = device.memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    if (allocation.memoryTypeIndex == Device::NO_MEMORY
        || !device.check(vk.AllocateMemory(device.handle(), &allocation, nullptr, memory))
        || !device.check(vk.BindImageMemory(device.handle(), *image, *memory, 0))) {
        return false;
    }

    const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = *image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = info.format,
            .subresourceRange =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
    };

    return device.check(vk.CreateImageView(device.handle(), &viewInfo, nullptr, view));
}

void drop_image(const Device &device, VkImage *image, VkDeviceMemory *memory, VkImageView *view) {
    const Api &vk = device.api();

    vk.DestroyImageView(device.handle(), *view, nullptr);
    vk.DestroyImage(device.handle(), *image, nullptr);
    vk.FreeMemory(device.handle(), *memory, nullptr);
    *view = VK_NULL_HANDLE;
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
}
}

VulkanPainter::VulkanPainter(std::unique_ptr<Device> device) : _device(std::move(device)) {
}

std::unique_ptr<VulkanPainter> VulkanPainter::create(SDL_Window *window, std::string *error) {
    std::unique_ptr<Device> device = Device::create(window, error);

    if (device == nullptr) {
        return nullptr;
    }

    std::unique_ptr<VulkanPainter> painter(new VulkanPainter(std::move(device)));

    painter->_window = window;

    const char *driver = SDL_GetCurrentVideoDriver();
    const bool wayland = driver != nullptr && std::string_view(driver) == "wayland";

#ifdef TIV_WAYLAND
    if (wayland) {
        painter->_colour = WaylandColor::create(window);
    }
#endif

    painter->_sdrOnly = wayland && painter->_colour == nullptr;
    painter->_swapchain = std::make_unique<Swapchain>(*painter->_device, painter->_colour != nullptr);

    if (!painter->start(error)) {
        return nullptr;
    }

    static_cast<void>(painter->remake_swapchain());

    return painter;
}

std::unique_ptr<VulkanPainter>
VulkanPainter::offscreen(const int width, const int height, const Swapchain::Output output, std::string *error) {
    std::unique_ptr<Device> device = Device::create(nullptr, error);

    if (device == nullptr) {
        return nullptr;
    }

    std::unique_ptr<VulkanPainter> painter(new VulkanPainter(std::move(device)));

    if (!painter->start(error)) {
        return nullptr;
    }

    if (!painter->make_offscreen(width, height, output)) {
        *error = "Vulkan: no offscreen target";

        return nullptr;
    }

    return painter;
}

bool VulkanPainter::start(std::string *error) {
    _staging = std::make_unique<Staging>(*_device);
    _pipelines = std::make_unique<Pipelines>(*_device);

    if (!_staging->valid() || !_pipelines->valid()) {
        *error = "Vulkan: no staging memory or pipeline layout";

        return false;
    }

    _pool = std::make_unique<TilePool>(*_device, *_staging, _pipelines->set_layout(), _pipelines->sampler());

    if (!_pool->valid()) {
        *error = "Vulkan: no descriptor pool for tiles";

        return false;
    }

    const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (Slot &slot : _slots) {
        if (!_device->check(_device->api().CreateSemaphore(_device->handle(), &semaphore, nullptr, &slot.available))
            || !grow(*_device, &slot.tiles, FIRST_BUFFER, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
            || !grow(*_device, &slot.ui, FIRST_BUFFER, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            *error = "Vulkan: no frame resources";

            return false;
        }
    }

    if (!make_atlas()) {
        *error = "Vulkan: no font atlas";

        return false;
    }

    return true;
}

VulkanPainter::~VulkanPainter() {
    const Api &vk = _device->api();
    VkDevice device = _device->handle();

    vk.DeviceWaitIdle(device);
    drop_framebuffers();
    drop_offscreen();

    for (Slot &slot : _slots) {
        vk.DestroySemaphore(device, slot.available, nullptr);
        slot.tiles = {};
        slot.ui = {};
    }

    vk.DestroyDescriptorPool(device, _atlasPool, nullptr);
    drop_image(*_device, &_atlas, &_atlasMemory, &_atlasView);
    _pool = nullptr;
    _swapchain = nullptr;
    _pipelines = nullptr;
    _staging = nullptr;
}

bool VulkanPainter::grow(Device &device, Buffer *buffer, const std::size_t bytes, const VkBufferUsageFlags usage) {
    if (buffer->valid() && buffer->size() >= bytes) {
        return true;
    }

    std::size_t size = std::max(FIRST_BUFFER, buffer->size());

    while (size < bytes) {
        size *= 2;
    }

    *buffer = Buffer(device, size, usage);

    return buffer->valid();
}

bool VulkanPainter::make_atlas() {
    const Api &vk = _device->api();
    const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {.width = ATLAS_WIDTH, .height = ATLAS_HEIGHT, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (!make_image(*_device, info, &_atlas, &_atlasMemory, &_atlasView)) {
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(ATLAS_WIDTH) * ATLAS_HEIGHT * Bitmap::CHANNELS);
    const auto light = [&](const int x, const int y) {
        const std::size_t at =
                ((static_cast<std::size_t>(y) * ATLAS_WIDTH) + static_cast<std::size_t>(x)) * Bitmap::CHANNELS;

        std::ranges::fill(std::span(pixels).subspan(at, Bitmap::CHANNELS), std::uint8_t{255});
    };

    for (int glyph = 0; glyph < Font::GLYPHS; ++glyph) {
        const std::span<const std::uint8_t, Font::SIZE> rows = Font::rows(glyph);
        const int left = static_cast<int>(cell_x(glyph)) + 1;
        const int top = static_cast<int>(cell_y(glyph)) + 1;

        for (int y = 0; y < Font::SIZE; ++y) {
            for (int x = 0; x < Font::SIZE; ++x) {
                if ((rows[static_cast<std::size_t>(y)] & (1U << static_cast<unsigned>(x))) != 0) {
                    light(left + x, top + y);
                }
            }
        }
    }

    for (int y = 0; y < CELL; ++y) {
        for (int x = 0; x < CELL; ++x) {
            light(static_cast<int>(cell_x(WHITE)) + x, static_cast<int>(cell_y(WHITE)) + y);
        }
    }

    if (!stage(*_device,
               *_staging,
               {
                       .image = _atlas,
                       .width = ATLAS_WIDTH,
                       .height = ATLAS_HEIGHT,
                       .from = VK_IMAGE_LAYOUT_UNDEFINED,
               },
               pixels.data(),
               static_cast<std::size_t>(ATLAS_WIDTH) * Bitmap::CHANNELS)) {
        return false;
    }

    const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1};
    const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
    };

    if (!_device->check(vk.CreateDescriptorPool(_device->handle(), &poolInfo, nullptr, &_atlasPool))) {
        return false;
    }

    VkDescriptorSetLayout layout = _pipelines->set_layout();
    const VkDescriptorSetAllocateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = _atlasPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
    };

    if (!_device->check(vk.AllocateDescriptorSets(_device->handle(), &setInfo, &_atlasSet))) {
        return false;
    }

    const VkDescriptorImageInfo bound{
            .sampler = _pipelines->sampler(),
            .imageView = _atlasView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = _atlasSet,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &bound,
    };

    vk.UpdateDescriptorSets(_device->handle(), 1, &write, 0, nullptr);

    return true;
}

bool VulkanPainter::make_offscreen(const int width, const int height, const Swapchain::Output output) {
    drop_offscreen();

    _offscreen.output = output;
    _offscreen.format =
            output == Swapchain::Output::Hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;

    const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = _offscreen.format,
            .extent =
                    {
                            .width = static_cast<std::uint32_t>(width),
                            .height = static_cast<std::uint32_t>(height),
                            .depth = 1,
                    },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (!make_image(*_device, info, &_offscreen.image, &_offscreen.memory, &_offscreen.view)) {
        return false;
    }

    const Pipelines::Kit *kit = _pipelines->kit(_offscreen.format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (kit == nullptr) {
        return false;
    }

    const VkFramebufferCreateInfo framebuffer{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = kit->pass,
            .attachmentCount = 1,
            .pAttachments = &_offscreen.view,
            .width = static_cast<std::uint32_t>(width),
            .height = static_cast<std::uint32_t>(height),
            .layers = 1,
    };

    _extent = {.width = static_cast<std::uint32_t>(width), .height = static_cast<std::uint32_t>(height)};

    return _device->check(
            _device->api().CreateFramebuffer(_device->handle(), &framebuffer, nullptr, &_offscreen.framebuffer));
}

void VulkanPainter::drop_offscreen() {
    if (_offscreen.image == VK_NULL_HANDLE) {
        return;
    }

    _device->api().DestroyFramebuffer(_device->handle(), _offscreen.framebuffer, nullptr);
    drop_image(*_device, &_offscreen.image, &_offscreen.memory, &_offscreen.view);
    _offscreen = {};
}

void VulkanPainter::drop_framebuffers() {
    for (VkFramebuffer framebuffer : _framebuffers) {
        _device->api().DestroyFramebuffer(_device->handle(), framebuffer, nullptr);
    }

    _framebuffers.clear();
    _framebufferPass = VK_NULL_HANDLE;
}

bool VulkanPainter::make_framebuffers(VkRenderPass pass) {
    if (_framebufferGeneration == _swapchain->generation() && _framebufferPass == pass) {
        return true;
    }

    drop_framebuffers();

    const VkExtent2D extent = _swapchain->extent();

    for (std::uint32_t index = 0; index < _swapchain->count(); ++index) {
        VkImageView view = _swapchain->view(index);
        const VkFramebufferCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = pass,
                .attachmentCount = 1,
                .pAttachments = &view,
                .width = extent.width,
                .height = extent.height,
                .layers = 1,
        };
        VkFramebuffer made = VK_NULL_HANDLE;

        if (!_device->check(_device->api().CreateFramebuffer(_device->handle(), &info, nullptr, &made))) {
            return false;
        }

        _framebuffers.push_back(made);
    }

    _framebufferGeneration = _swapchain->generation();
    _framebufferPass = pass;

    return true;
}

bool VulkanPainter::remake_swapchain() {
    int width = 0;
    int height = 0;

    SDL_GetWindowSizeInPixels(_window, &width, &height);

    if (width <= 0 || height <= 0) {
        return false;
    }

    // A frame in flight may still use the framebuffers.
    _device->api().DeviceWaitIdle(_device->handle());
    drop_framebuffers();

    // Mesa describes every swapchain but a pass-through one, and two descriptions close the window.
    if (_colour != nullptr && (_wanted == Swapchain::Output::Sdr || !supports(Swapchain::Output::Hdr10))) {
        _colour->forget();
    }

    if (!_swapchain->make(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), _wanted)) {
        return false;
    }

    describe();

    return true;
}

void VulkanPainter::describe() {
    if (_swapchain == nullptr || _swapchain->output() != Swapchain::Output::Hdr10) {
        return;
    }

    if (_colour != nullptr) {
        // A refused description would leave PQ on a surface the compositor reads as sRGB.
        // The next frame makes the SDR swapchain.
        if (!_colour->describe(_hdr.minimum, _hdr.peak)) {
            _sdrOnly = true;
            _wanted = Swapchain::Output::Sdr;
            _stale = true;
        }
    } else {
        _swapchain->set_metadata(_hdr.minimum, _hdr.peak);
    }
}

void VulkanPainter::set_hdr(const Hdr &hdr) {
    const bool moved = hdr.minimum != _hdr.minimum || hdr.peak != _hdr.peak;

    _hdr = hdr;

    if (moved) {
        describe();
    }
}

const WaylandColor::Output *VulkanPainter::wayland_output() {
    return _colour != nullptr ? &_colour->output() : nullptr;
}

// The bars and text are sRGB colours. On a PQ output they are encoded for white at the white of the images.
Colour VulkanPainter::shown(const Colour colour) const {
    if (output() != Swapchain::Output::Hdr10) {
        return colour;
    }

    const auto encode = [&](const std::uint8_t value) {
        const float signal = Tone::pq_signal(Tone::from_srgb(value) * _hdr.whiteNits);

        return static_cast<std::uint8_t>(std::lround(std::clamp(signal, 0.0F, 1.0F) * 255.0F));
    };

    return {.r = encode(colour.r), .g = encode(colour.g), .b = encode(colour.b), .a = colour.a};
}

bool VulkanPainter::supports(const Swapchain::Output output) const {
    if (_swapchain == nullptr) {
        return output == _offscreen.output;
    }

    return (output == Swapchain::Output::Sdr || !_sdrOnly) && _swapchain->supports(output);
}

bool VulkanPainter::set_output(const Swapchain::Output output) {
    if (_swapchain == nullptr) {
        return output == _offscreen.output;
    }

    if (!supports(output)) {
        return false;
    }

    if (output != _wanted) {
        _wanted = output;
        static_cast<void>(remake_swapchain());
    }

    return true;
}

Swapchain::Output VulkanPainter::output() const {
    return _swapchain != nullptr ? _swapchain->output() : _offscreen.output;
}

bool VulkanPainter::waits_for_display() const {
    return _swapchain != nullptr && !_swapchain->mailbox();
}

std::size_t VulkanPainter::resident_bytes() const {
    return _pool->resident_bytes();
}

int VulkanPainter::max_tile() const {
    return static_cast<int>(std::min<std::uint32_t>(TilePool::SIDE, _device->limits().maxImageDimension2D));
}

bool VulkanPainter::begin_frame(const Colour clear) {
    if (_device->lost()) {
        return false;
    }

    _pool->trim();

    const Slot &slot = _slots.at(_slot);

    _device->wait(slot.serial);

    if (_swapchain != nullptr) {
        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(_window, &width, &height);

        const VkExtent2D current = _swapchain->extent();
        const bool resized = std::cmp_not_equal(width, current.width) || std::cmp_not_equal(height, current.height);

        if ((!_swapchain->made() || resized || std::exchange(_stale, false)) && !remake_swapchain()) {
            return false;
        }

        Swapchain::Acquired acquired = _swapchain->acquire(slot.available, &_image);

        if (acquired == Swapchain::Acquired::Stale) {
            if (!remake_swapchain()) {
                return false;
            }

            acquired = _swapchain->acquire(slot.available, &_image);
        }

        if (acquired != Swapchain::Acquired::Ready) {
            return false;
        }

        _extent = _swapchain->extent();
    }

    _kit = _pipelines->kit(_swapchain != nullptr ? _swapchain->format() : _offscreen.format,
                           _swapchain != nullptr ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                 : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // A frame that fails after the acquire would hold its image for good.
    if (_kit == nullptr || (_swapchain != nullptr && !make_framebuffers(_kit->pass))) {
        _device->fail();

        return false;
    }

    const Colour background = shown(clear);

    _clear = {
            .float32 =
                    {
                            static_cast<float>(background.r) / 255.0F,
                            static_cast<float>(background.g) / 255.0F,
                            static_cast<float>(background.b) / 255.0F,
                            static_cast<float>(background.a) / 255.0F,
                    },
    };
    _push.width = static_cast<float>(_extent.width);
    _push.height = static_cast<float>(_extent.height);
    _tileData.clear();
    _uiData.clear();
    _batches.clear();
    set_view(nullptr);
    _drawing = true;

    return true;
}

void VulkanPainter::set_view(const Rect *view) {
    if (view == nullptr) {
        _originX = 0.0F;
        _originY = 0.0F;
        _scissor = {.offset = {.x = 0, .y = 0}, .extent = _extent};

        return;
    }

    const auto x0 = static_cast<std::int32_t>(std::lround(std::max(view->x, 0.0)));
    const auto y0 = static_cast<std::int32_t>(std::lround(std::max(view->y, 0.0)));
    const auto x1 =
            static_cast<std::int32_t>(std::lround(std::min(view->x + view->width, static_cast<double>(_extent.width))));
    const auto y1 = static_cast<std::int32_t>(
            std::lround(std::min(view->y + view->height, static_cast<double>(_extent.height))));

    _originX = static_cast<float>(view->x);
    _originY = static_cast<float>(view->y);
    _scissor = {
            .offset = {.x = x0, .y = y0},
            .extent =
                    {
                            .width = static_cast<std::uint32_t>(std::max(x1 - x0, 0)),
                            .height = static_cast<std::uint32_t>(std::max(y1 - y0, 0)),
                    },
    };
}

void VulkanPainter::add(const Kind kind, const std::uint32_t slab, const std::uint32_t count) {
    const auto first = static_cast<std::uint32_t>(kind == Kind::Tiles ? _tileData.size() : _uiData.size()) - count;

    if (!_batches.empty()) {
        Batch &last = _batches.back();

        if (last.kind == kind && last.slab == slab && last.first + last.count == first
            && last.scissor.offset.x == _scissor.offset.x && last.scissor.offset.y == _scissor.offset.y
            && last.scissor.extent.width == _scissor.extent.width
            && last.scissor.extent.height == _scissor.extent.height) {
            last.count += count;

            return;
        }
    }

    _batches.push_back({
            .kind = kind,
            .slab = slab,
            .first = first,
            .count = count,
            .scissor = _scissor,
    });
}

// The orientation turns the source into the target: mirror first, then quarter turns clockwise. Each
// corner of the source goes where that puts it.
void VulkanPainter::tile(const Tile tile, const Rect &source, const Rect &target, const int orientation) {
    if (tile == NO_TILE) {
        return;
    }

    const TilePool::Place place = _pool->place(tile);
    TileInstance instance{
            .target =
                    {
                            static_cast<float>(target.x) + _originX,
                            static_cast<float>(target.y) + _originY,
                            static_cast<float>(target.width),
                            static_cast<float>(target.height),
                    },
            .bounds =
                    {
                            static_cast<float>(place.x),
                            static_cast<float>(place.y),
                            static_cast<float>(place.x + place.width),
                            static_cast<float>(place.y + place.height),
                    },
            .layer = static_cast<float>(place.layer),
    };
    const int quarters = Orient::quarters(orientation);
    const bool mirrors = Orient::mirrors(orientation);

    for (int corner = 0; corner < 4; ++corner) {
        const int u = static_cast<int>(static_cast<unsigned>(corner) & 1U);
        const int v = static_cast<int>(static_cast<unsigned>(corner) >> 1U);
        int x = mirrors ? 1 - u : u;
        int y = v;

        for (int turn = 0; turn < quarters; ++turn) {
            x = std::exchange(y, x);
            x = 1 - x;
        }

        const auto at = static_cast<std::size_t>((y * 2) + x) * 2;

        instance.corners.at(at) = static_cast<float>(place.x + source.x + (u * source.width));
        instance.corners.at(at + 1) = static_cast<float>(place.y + source.y + (v * source.height));
    }

    _tileData.push_back(instance);
    add(Kind::Tiles, place.slab, 1);
}

void VulkanPainter::ui_quad(const float x0,
                            const float y0,
                            const float x1,
                            const float y1,
                            const float u0,
                            const float v0,
                            const float u1,
                            const float v1,
                            const std::uint32_t colour) {
    const UiVertex a{.x = x0 + _originX, .y = y0 + _originY, .u = u0, .v = v0, .colour = colour};
    const UiVertex b{.x = x1 + _originX, .y = y0 + _originY, .u = u1, .v = v0, .colour = colour};
    const UiVertex c{.x = x0 + _originX, .y = y1 + _originY, .u = u0, .v = v1, .colour = colour};
    const UiVertex d{.x = x1 + _originX, .y = y1 + _originY, .u = u1, .v = v1, .colour = colour};

    _uiData.insert(_uiData.end(), {a, b, c, c, b, d});
    add(Kind::Ui, 0, 6);
}

void VulkanPainter::fill(const Rect &rect, const Colour colour) {
    const float u = cell_x(WHITE) + (CELL / 2.0F);
    const float v = cell_y(WHITE) + (CELL / 2.0F);

    ui_quad(static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.x + rect.width),
            static_cast<float>(rect.y + rect.height),
            u,
            v,
            u,
            v,
            packed(shown(colour)));
}

void VulkanPainter::triangles(const std::span<const Point> points, const Colour colour) {
    const float u = cell_x(WHITE) + (CELL / 2.0F);
    const float v = cell_y(WHITE) + (CELL / 2.0F);
    const std::uint32_t shade = packed(shown(colour));
    const std::size_t count = points.size() / 3 * 3;

    for (const Point &point : points.first(count)) {
        _uiData.push_back({.x = point.x + _originX, .y = point.y + _originY, .u = u, .v = v, .colour = shade});
    }

    if (count > 0) {
        add(Kind::Ui, 0, static_cast<std::uint32_t>(count));
    }
}

void VulkanPainter::text(const float x, const float y, const float scale, std::string_view text, const Colour colour) {
    const float side = Font::SIZE * scale;
    const std::uint32_t shade = packed(shown(colour));
    float left = x;

    while (!text.empty()) {
        const int glyph = Font::glyph(Font::next(text));

        if (glyph >= 0) {
            const float u = cell_x(glyph) + 1.0F;
            const float v = cell_y(glyph) + 1.0F;

            ui_quad(left, y, left + side, y + side, u, v, u + Font::SIZE, v + Font::SIZE, shade);
        }

        left += side;
    }
}

Painter::Tile VulkanPainter::acquire_tile(const int width, const int height, const Bitmap::Encoding encoding) {
    return _pool->acquire(width, height, encoding);
}

void VulkanPainter::release_tile(const Tile tile) {
    _pool->release(tile);
}

void VulkanPainter::upload(const Tile tile, const std::uint8_t *pixels, const std::size_t pitch) {
    if (tile == NO_TILE) {
        return;
    }

    const TilePool::Place place = _pool->place(tile);

    static_cast<void>(stage(*_device,
                            *_staging,
                            {
                                    .image = _pool->image(place.slab),
                                    .layer = place.layer,
                                    .x = place.x,
                                    .y = place.y,
                                    .width = place.width,
                                    .height = place.height,
                            },
                            pixels,
                            pitch));
}

void VulkanPainter::record(VkCommandBuffer commands, const Slot &slot, const Pipelines::Kit &kit) {
    const Api &vk = _device->api();
    std::optional<Kind> boundKind;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    const VkDeviceSize zero = 0;

    vk.CmdPushConstants(commands, _pipelines->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &_push);

    for (const Batch &batch : _batches) {
        if (batch.scissor.extent.width == 0 || batch.scissor.extent.height == 0) {
            continue;
        }

        if (boundKind != batch.kind) {
            VkBuffer vertices = batch.kind == Kind::Tiles ? slot.tiles.handle() : slot.ui.handle();

            vk.CmdBindPipeline(
                    commands, VK_PIPELINE_BIND_POINT_GRAPHICS, batch.kind == Kind::Tiles ? kit.tile : kit.ui);
            vk.CmdBindVertexBuffers(commands, 0, 1, &vertices, &zero);
            boundKind = batch.kind;
        }

        VkDescriptorSet set = batch.kind == Kind::Tiles ? _pool->set(batch.slab) : _atlasSet;

        if (set != boundSet) {
            vk.CmdBindDescriptorSets(
                    commands, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelines->layout(), 0, 1, &set, 0, nullptr);
            boundSet = set;
        }

        vk.CmdSetScissor(commands, 0, 1, &batch.scissor);

        if (batch.kind == Kind::Tiles) {
            vk.CmdDraw(commands, 4, batch.count, 0, batch.first);
        } else {
            vk.CmdDraw(commands, batch.count, 1, batch.first, 0);
        }
    }
}

void VulkanPainter::end_frame() {
    if (!_drawing) {
        return;
    }

    _drawing = false;

    Slot &slot = _slots.at(_slot);
    const Api &vk = _device->api();

    if (!grow(*_device, &slot.tiles, _tileData.size() * sizeof(TileInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        || !grow(*_device, &slot.ui, _uiData.size() * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        _device->fail();

        return;
    }

    if (!_tileData.empty()) {
        std::memcpy(slot.tiles.bytes().data(), _tileData.data(), _tileData.size() * sizeof(TileInstance));
    }

    if (!_uiData.empty()) {
        std::memcpy(slot.ui.bytes().data(), _uiData.data(), _uiData.size() * sizeof(UiVertex));
    }

    _staging->submit();

    const bool windowed = _swapchain != nullptr;
    const Pipelines::Kit &kit = *_kit;
    VkCommandBuffer commands = _device->begin();

    if (commands == VK_NULL_HANDLE) {
        _device->fail();

        return;
    }

    const VkClearValue clear{.color = _clear};
    const VkRenderPassBeginInfo pass{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = kit.pass,
            .framebuffer = windowed ? _framebuffers.at(_image) : _offscreen.framebuffer,
            .renderArea = {.offset = {.x = 0, .y = 0}, .extent = _extent},
            .clearValueCount = 1,
            .pClearValues = &clear,
    };
    const VkViewport viewport{
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<float>(_extent.width),
            .height = static_cast<float>(_extent.height),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
    };

    vk.CmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vk.CmdSetViewport(commands, 0, 1, &viewport);
    record(commands, slot, kit);

    vk.CmdEndRenderPass(commands);

    if (windowed) {
        slot.serial = _device->submit(
                commands, slot.available, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, _swapchain->rendered(_image));

        if (_device->lost()) {
            return;
        }

        if (!_swapchain->present(_image)) {
            static_cast<void>(remake_swapchain());
        }
    } else {
        slot.serial = _device->submit(commands);
    }

    _slot = (_slot + 1) % _slots.size();
}

std::vector<std::uint8_t> VulkanPainter::read() {
    std::vector<std::uint8_t> pixels;

    if (_offscreen.image == VK_NULL_HANDLE) {
        return pixels;
    }

    const std::size_t bytes = static_cast<std::size_t>(_extent.width) * _extent.height * Bitmap::CHANNELS;
    const Buffer readback(*_device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer commands = _device->begin();

    if (!readback.valid() || commands == VK_NULL_HANDLE) {
        return pixels;
    }

    const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = _extent.width, .height = _extent.height, .depth = 1},
    };

    _device->api().CmdCopyImageToBuffer(
            commands, _offscreen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle(), 1, &region);
    _device->wait(_device->submit(commands));
    pixels.assign(readback.bytes().begin(), readback.bytes().end());

    return pixels;
}
}
