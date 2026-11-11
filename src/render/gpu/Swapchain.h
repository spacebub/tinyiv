// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_SWAPCHAIN_H
#define TIV_RENDER_GPU_SWAPCHAIN_H


#include <cstdint>
#include <vector>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
class Swapchain {

public:
    enum class Output : std::uint8_t {
        Sdr,
        Hdr10,
    };

    enum class Acquired : std::uint8_t {
        Ready,
        // Out of date or the wrong size. Remake, then try again.
        Stale,
        Failed,
    };

    // With passThrough, HDR goes out as PQ on a surface the application describes.
    // Without it, HDR goes out as HDR10.
    Swapchain(Device &device, bool passThrough);
    ~Swapchain();

    Swapchain(const Swapchain &) = delete;
    Swapchain(Swapchain &&) = delete;
    Swapchain &operator=(const Swapchain &) = delete;
    Swapchain &operator=(Swapchain &&) = delete;

    // The width and height count only where the surface leaves the size to the caller, as Wayland does.
    // False for a surface with no area, or on failure.
    bool make(std::uint32_t width, std::uint32_t height, Output output);

    [[nodiscard]] bool supports(Output output) const;
    [[nodiscard]] bool made() const { return _swapchain != VK_NULL_HANDLE; }
    [[nodiscard]] Output output() const { return _output; }
    [[nodiscard]] VkFormat format() const { return _format; }
    [[nodiscard]] VkExtent2D extent() const { return _extent; }
    [[nodiscard]] bool mailbox() const { return _mailbox; }
    [[nodiscard]] std::uint32_t generation() const { return _generation; }
    [[nodiscard]] std::size_t count() const { return _images.size(); }
    [[nodiscard]] VkImageView view(std::uint32_t index) const { return _views.at(index); }
    [[nodiscard]] VkSemaphore rendered(std::uint32_t index) const { return _rendered.at(index); }

    // For an HDR10 output that is not passed through, as Windows takes it. Nothing otherwise.
    void set_metadata(float minimum, float peak);

    Acquired acquire(VkSemaphore available, std::uint32_t *index);
    // False when the swapchain wants remaking.
    bool present(std::uint32_t index);

private:
    void drop_images();
    [[nodiscard]] VkSurfaceFormatKHR wanted(Output output) const;

    Device *_device;
    bool _passThrough;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    Output _output = Output::Sdr;
    VkFormat _format = VK_FORMAT_UNDEFINED;
    VkExtent2D _extent{};
    bool _mailbox = false;
    std::uint32_t _generation = 0;
    std::vector<VkImage> _images;
    std::vector<VkImageView> _views;
    // One for each swapchain image. An image is known to be free only when it is acquired again.
    std::vector<VkSemaphore> _rendered;
};
}


#endif //TIV_RENDER_GPU_SWAPCHAIN_H
