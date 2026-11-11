// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_DEVICE_H
#define TIV_RENDER_GPU_DEVICE_H


#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "render/gpu/Api.h"

namespace tiv::gpu {
class Device {

public:
    static constexpr std::uint32_t NO_MEMORY = ~std::uint32_t{0};

    // The window needs SDL_WINDOW_VULKAN. A null window gives a device for offscreen drawing.
    [[nodiscard]] static std::unique_ptr<Device> create(SDL_Window *window, std::string *error);

    ~Device();

    Device(const Device &) = delete;
    Device(Device &&) = delete;
    Device &operator=(const Device &) = delete;
    Device &operator=(Device &&) = delete;

    [[nodiscard]] const Api &api() const { return _api; }
    [[nodiscard]] VkPhysicalDevice physical() const { return _physical; }
    [[nodiscard]] VkDevice handle() const { return _device; }
    [[nodiscard]] VkQueue queue() const { return _queue; }
    [[nodiscard]] VkSurfaceKHR surface() const { return _surface; }
    [[nodiscard]] const VkPhysicalDeviceLimits &limits() const { return _limits; }
    [[nodiscard]] const std::string &name() const { return _name; }

    [[nodiscard]] bool colour_spaces() const { return _colourSpaces; }
    [[nodiscard]] bool hdr_metadata() const { return _api.SetHdrMetadataEXT != nullptr; }

    // Prefers a type without the avoided properties, and takes one with them when there is no other.
    [[nodiscard]] std::uint32_t
    memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags avoided = 0) const;

    bool check(VkResult result);
    [[nodiscard]] bool lost() const { return _lost; }
    // Stops all drawing after a failure the device cannot recover from.
    void fail() { _lost = true; }

    [[nodiscard]] static int validation_messages();

    // Every submission takes the next serial on one timeline, so one number says what the GPU finished.
    [[nodiscard]] VkCommandBuffer begin();
    std::uint64_t submit(VkCommandBuffer commands,
                         VkSemaphore wait = VK_NULL_HANDLE,
                         VkPipelineStageFlags waitStage = 0,
                         VkSemaphore signal = VK_NULL_HANDLE);
    [[nodiscard]] std::uint64_t submitted() const { return _submitted; }
    [[nodiscard]] std::uint64_t completed();
    void wait(std::uint64_t serial);

private:
    Device() = default;

    bool make_instance(SDL_Window *window, std::string *error);
    bool pick(std::string *error);
    bool make_device(std::string *error);

    struct Recorder {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::uint64_t serial = 0;
    };

    Api _api;
    VkInstance _instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger = VK_NULL_HANDLE;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE;
    std::uint32_t _family = 0;
    VkPhysicalDeviceLimits _limits{};
    VkPhysicalDeviceMemoryProperties _memory{};
    std::string _name;
    bool _colourSpaces = false;
    bool _wantsHdrMetadata = false;
    bool _lost = false;
    bool _loaded = false;
    VkSemaphore _timeline = VK_NULL_HANDLE;
    std::uint64_t _submitted = 0;
    std::uint64_t _completed = 0;
    VkCommandPool _pool = VK_NULL_HANDLE;
    std::vector<Recorder> _recorders;
};
}


#endif //TIV_RENDER_GPU_DEVICE_H
