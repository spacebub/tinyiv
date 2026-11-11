// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_VULKANPAINTER_H
#define TIV_RENDER_GPU_VULKANPAINTER_H


#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Tone.h"
#include "render/Painter.h"
#include "render/WaylandColor.h"
#include "render/gpu/Buffer.h"
#include "render/gpu/Device.h"
#include "render/gpu/Pipelines.h"
#include "render/gpu/Staging.h"
#include "render/gpu/Swapchain.h"
#include "render/gpu/TilePool.h"

namespace tiv::gpu {
class VulkanPainter final : public Painter {

public:
    // The window needs SDL_WINDOW_VULKAN.
    [[nodiscard]] static std::unique_ptr<VulkanPainter> create(SDL_Window *window, std::string *error);
    [[nodiscard]] static std::unique_ptr<VulkanPainter>
    offscreen(int width, int height, Swapchain::Output output, std::string *error);

    ~VulkanPainter() override;

    VulkanPainter(const VulkanPainter &) = delete;
    VulkanPainter(VulkanPainter &&) = delete;
    VulkanPainter &operator=(const VulkanPainter &) = delete;
    VulkanPainter &operator=(VulkanPainter &&) = delete;

    bool begin_frame(Colour clear) override;
    void end_frame() override;
    void set_view(const Rect *view) override;
    void tile(Tile tile, const Rect &source, const Rect &target, int orientation) override;
    void fill(const Rect &rect, Colour colour) override;
    void triangles(std::span<const Point> points, Colour colour) override;
    void text(float x, float y, float scale, std::string_view text, Colour colour) override;
    [[nodiscard]] Tile acquire_tile(int width, int height, Bitmap::Encoding encoding) override;
    void release_tile(Tile tile) override;
    void upload(Tile tile, const std::uint8_t *pixels, std::size_t pitch) override;
    [[nodiscard]] int max_tile() const override;

    struct Hdr {
        float minimum = 0.0F;
        float peak = 0.0F;
        // Nits that PQ encodes SDR white at, for the bars and text.
        float whiteNits = Tone::PQ_REFERENCE_NITS;
    };

    // Remakes the swapchain at once. False when the surface has no HDR10.
    bool set_output(Swapchain::Output output);
    void set_hdr(const Hdr &hdr);
    // Null off Wayland, where the window properties of SDL describe the display.
    [[nodiscard]] const WaylandColor::Output *wayland_output();
    [[nodiscard]] bool supports(Swapchain::Output output) const;
    [[nodiscard]] Swapchain::Output output() const;
    [[nodiscard]] bool waits_for_display() const;
    [[nodiscard]] bool lost() const { return _device->lost(); }
    [[nodiscard]] const std::string &device_name() const { return _device->name(); }
    [[nodiscard]] std::size_t resident_bytes() const;

    // The last frame drawn offscreen, four bytes a pixel in the format of the output, rows packed.
    [[nodiscard]] std::vector<std::uint8_t> read();

private:
    enum class Kind : std::uint8_t {
        Tiles,
        Ui,
    };

    struct Batch {
        Kind kind = Kind::Ui;
        std::uint32_t slab = 0;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        VkRect2D scissor{};
    };

    struct Slot {
        VkSemaphore available = VK_NULL_HANDLE;
        Buffer tiles;
        Buffer ui;
        std::uint64_t serial = 0;
    };

    struct Offscreen {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        Swapchain::Output output = Swapchain::Output::Sdr;
    };

    explicit VulkanPainter(std::unique_ptr<Device> device);

    [[nodiscard]] bool start(std::string *error);
    [[nodiscard]] bool make_atlas();
    [[nodiscard]] bool make_offscreen(int width, int height, Swapchain::Output output);
    void drop_offscreen();
    [[nodiscard]] bool remake_swapchain();
    [[nodiscard]] bool make_framebuffers(VkRenderPass pass);
    void drop_framebuffers();
    void record(VkCommandBuffer commands, const Slot &slot, const Pipelines::Kit &kit);
    void add(Kind kind, std::uint32_t slab, std::uint32_t count);
    void ui_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, std::uint32_t colour);
    void describe();
    [[nodiscard]] Colour shown(Colour colour) const;
    [[nodiscard]] static bool grow(Device &device, Buffer *buffer, std::size_t bytes, VkBufferUsageFlags usage);

    std::unique_ptr<Device> _device;
    SDL_Window *_window = nullptr;
    std::unique_ptr<Staging> _staging;
    std::unique_ptr<Pipelines> _pipelines;
    std::unique_ptr<TilePool> _pool;
    std::unique_ptr<WaylandColor> _colour;
    // Set on Wayland when the colour manager cannot take PQ with luminances.
    // HDR would then be tone mapped from 10000 nits.
    bool _sdrOnly = false;
    bool _stale = false;
    Hdr _hdr;
    std::unique_ptr<Swapchain> _swapchain;
    Swapchain::Output _wanted = Swapchain::Output::Sdr;
    Offscreen _offscreen;

    VkImage _atlas = VK_NULL_HANDLE;
    VkDeviceMemory _atlasMemory = VK_NULL_HANDLE;
    VkImageView _atlasView = VK_NULL_HANDLE;
    VkDescriptorPool _atlasPool = VK_NULL_HANDLE;
    VkDescriptorSet _atlasSet = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> _framebuffers;
    std::uint32_t _framebufferGeneration = 0;
    VkRenderPass _framebufferPass = VK_NULL_HANDLE;

    std::array<Slot, 2> _slots;
    std::size_t _slot = 0;
    std::uint32_t _image = 0;
    bool _drawing = false;
    VkClearColorValue _clear{};
    VkExtent2D _extent{};
    const Pipelines::Kit *_kit = nullptr;
    Push _push;

    std::vector<TileInstance> _tileData;
    std::vector<UiVertex> _uiData;
    std::vector<Batch> _batches;
    float _originX = 0.0F;
    float _originY = 0.0F;
    VkRect2D _scissor{};
};
}


#endif //TIV_RENDER_GPU_VULKANPAINTER_H
