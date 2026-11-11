// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"
#include "render/gpu/Swapchain.h"

namespace tiv::gpu {
namespace {
constexpr VkSurfaceFormatKHR SDR{
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
};
constexpr VkSurfaceFormatKHR HDR10{
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT,
};
// The same PQ words, which the driver leaves for the application to describe.
constexpr VkSurfaceFormatKHR PASS_THROUGH{
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .colorSpace = VK_COLOR_SPACE_PASS_THROUGH_EXT,
};

std::vector<VkSurfaceFormatKHR> formats(const Device &device) {
    const Api &vk = device.api();
    std::uint32_t count = 0;

    vk.GetPhysicalDeviceSurfaceFormatsKHR(device.physical(), device.surface(), &count, nullptr);

    std::vector<VkSurfaceFormatKHR> all(count);

    vk.GetPhysicalDeviceSurfaceFormatsKHR(device.physical(), device.surface(), &count, all.data());

    return all;
}

bool offers(const std::vector<VkSurfaceFormatKHR> &all, const VkSurfaceFormatKHR &want) {
    return std::ranges::any_of(all, [&](const VkSurfaceFormatKHR &each) {
        return each.format == want.format && each.colorSpace == want.colorSpace;
    });
}
}

Swapchain::Swapchain(Device &device, const bool passThrough) : _device(&device), _passThrough(passThrough) {
}

VkSurfaceFormatKHR Swapchain::wanted(const Output output) const {
    if (output == Output::Sdr) {
        return SDR;
    }

    return _passThrough ? PASS_THROUGH : HDR10;
}

Swapchain::~Swapchain() {
    drop_images();

    if (_swapchain != VK_NULL_HANDLE) {
        _device->api().DestroySwapchainKHR(_device->handle(), _swapchain, nullptr);
    }
}

bool Swapchain::supports(const Output output) const {
    return output == Output::Sdr || (_device->colour_spaces() && offers(formats(*_device), wanted(output)));
}

void Swapchain::drop_images() {
    const Api &vk = _device->api();

    for (VkImageView view : _views) {
        vk.DestroyImageView(_device->handle(), view, nullptr);
    }

    for (VkSemaphore semaphore : _rendered) {
        vk.DestroySemaphore(_device->handle(), semaphore, nullptr);
    }

    _views.clear();
    _rendered.clear();
    _images.clear();
}

bool Swapchain::make(const std::uint32_t width, const std::uint32_t height, const Output output) {
    const Api &vk = _device->api();
    VkDevice device = _device->handle();
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): filled by the query below.
    VkSurfaceCapabilitiesKHR caps{};

    if (!_device->check(vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(_device->physical(), _device->surface(), &caps))) {
        return false;
    }

    VkExtent2D extent = caps.currentExtent;

    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        extent = {
                .width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                .height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height),
        };
    }

    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    const std::vector<VkSurfaceFormatKHR> all = formats(*_device);
    VkSurfaceFormatKHR chosen = wanted(output);

    if (!offers(all, chosen)) {
        chosen = SDR;

        // Another UNORM format stores values as they are. An _SRGB one would encode them a second time.
        const auto plain = std::ranges::find_if(all, [](const VkSurfaceFormatKHR &each) {
            return each.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                   && (each.format == VK_FORMAT_R8G8B8A8_UNORM || each.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        });

        if (!offers(all, chosen) && plain != all.end()) {
            chosen = *plain;
        } else if (!offers(all, chosen) && !all.empty()) {
            chosen = all.front();
        }
    }

    std::uint32_t modeCount = 0;
    vk.GetPhysicalDeviceSurfacePresentModesKHR(_device->physical(), _device->surface(), &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vk.GetPhysicalDeviceSurfacePresentModesKHR(_device->physical(), _device->surface(), &modeCount, modes.data());

    const bool mailbox = std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
    std::uint32_t images = caps.minImageCount + 1;

    if (caps.maxImageCount != 0) {
        images = std::min(images, caps.maxImageCount);
    }

    const VkCompositeAlphaFlagBitsKHR alpha =
            (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                    : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha
                                                               & (~caps.supportedCompositeAlpha + 1));
    VkSwapchainKHR old = _swapchain;
    const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = _device->surface(),
            .minImageCount = images,
            .imageFormat = chosen.format,
            .imageColorSpace = chosen.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage =
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha = alpha,
            .presentMode = mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
            .oldSwapchain = old,
    };

    // Views and semaphores of the old images may still be in use by a frame in flight.
    vk.DeviceWaitIdle(device);
    drop_images();

    const VkResult result = vk.CreateSwapchainKHR(device, &info, nullptr, &_swapchain);

    if (old != VK_NULL_HANDLE) {
        vk.DestroySwapchainKHR(device, old, nullptr);
    }

    if (!_device->check(result)) {
        _swapchain = VK_NULL_HANDLE;

        return false;
    }

    std::uint32_t count = 0;
    vk.GetSwapchainImagesKHR(device, _swapchain, &count, nullptr);
    _images.resize(count);
    vk.GetSwapchainImagesKHR(device, _swapchain, &count, _images.data());

    for (VkImage image : _images) {
        const VkImageViewCreateInfo view{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = chosen.format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        _views.push_back(VK_NULL_HANDLE);
        _rendered.push_back(VK_NULL_HANDLE);

        if (!_device->check(vk.CreateImageView(device, &view, nullptr, &_views.back()))
            || !_device->check(vk.CreateSemaphore(device, &semaphore, nullptr, &_rendered.back()))) {
            drop_images();
            vk.DestroySwapchainKHR(device, _swapchain, nullptr);
            _swapchain = VK_NULL_HANDLE;

            return false;
        }
    }

    _output = chosen.format == wanted(Output::Hdr10).format && chosen.colorSpace == wanted(Output::Hdr10).colorSpace
                      ? Output::Hdr10
                      : Output::Sdr;
    _format = chosen.format;
    _extent = extent;
    _mailbox = mailbox;
    ++_generation;

    return true;
}

Swapchain::Acquired Swapchain::acquire(VkSemaphore available, std::uint32_t *index) {
    if (_swapchain == VK_NULL_HANDLE) {
        return Acquired::Stale;
    }

    const VkResult result = _device->api().AcquireNextImageKHR(
            _device->handle(), _swapchain, std::numeric_limits<std::uint64_t>::max(), available, VK_NULL_HANDLE, index);

    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return Acquired::Ready;
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return Acquired::Stale;
    }

    _device->check(result);

    return Acquired::Failed;
}

bool Swapchain::present(const std::uint32_t index) {
    VkSemaphore wait = _rendered.at(index);
    const VkPresentInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait,
            .swapchainCount = 1,
            .pSwapchains = &_swapchain,
            .pImageIndices = &index,
    };
    const VkResult result = _device->api().QueuePresentKHR(_device->queue(), &info);

    // Suboptimal still shows the frame. A change of size is caught when the next frame begins.
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return true;
    }

    _device->check(result);

    return false;
}

void Swapchain::set_metadata(const float minimum, const float peak) {
    if (_passThrough || _output != Output::Hdr10 || _swapchain == VK_NULL_HANDLE || !_device->hdr_metadata()) {
        return;
    }

    // The primaries and white point of BT.2020, which PQ tiles are encoded in.
    const VkHdrMetadataEXT metadata{
            .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
            .displayPrimaryRed = {.x = 0.708F, .y = 0.292F},
            .displayPrimaryGreen = {.x = 0.170F, .y = 0.797F},
            .displayPrimaryBlue = {.x = 0.131F, .y = 0.046F},
            .whitePoint = {.x = 0.3127F, .y = 0.3290F},
            .maxLuminance = peak,
            .minLuminance = minimum,
            .maxContentLightLevel = peak,
            // Unknown, which a frame of images rarely comes near.
            .maxFrameAverageLightLevel = 0.0F,
    };

    _device->api().SetHdrMetadataEXT(_device->handle(), 1, &_swapchain, &metadata);
}
}
