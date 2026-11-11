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
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "render/gpu/Api.h"
#include "render/gpu/Device.h"

namespace tiv::gpu {
namespace {
constexpr std::string_view VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

int score_of(const VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 2;
        default:
            return 1;
    }
}

std::atomic<int> &messages() {
    static std::atomic<int> count{0};

    return count;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
report(const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
       [[maybe_unused]] const VkDebugUtilsMessageTypeFlagsEXT type,
       const VkDebugUtilsMessengerCallbackDataEXT *data,
       // NOLINTNEXTLINE(misc-const-correctness): the callback signature is fixed by Vulkan.
       [[maybe_unused]] void *user) {
    if ((severity
         & (static_cast<VkDebugUtilsMessageSeverityFlagsEXT>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
        != 0) {
        messages().fetch_add(1);
        std::println(stderr, "vulkan: {}", data->pMessage);
    }

    return VK_FALSE;
}

template <typename Properties>
bool listed(const std::vector<Properties> &all, const std::string_view name) {
    return std::ranges::any_of(all, [&](const Properties &each) {
        if constexpr (requires { each.extensionName; }) {
            return name == static_cast<const char *>(each.extensionName);
        } else {
            return name == static_cast<const char *>(each.layerName);
        }
    });
}

template <typename Function, typename Generic>
Function as(Generic function) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): entry points come back as one generic type.
    return reinterpret_cast<Function>(function);
}

std::string failed(const char *what, const VkResult result) {
    return std::string(what) + " failed (" + std::to_string(static_cast<int>(result)) + ")";
}
}

std::unique_ptr<Device> Device::create(SDL_Window *window, std::string *error) {
    std::unique_ptr<Device> device(new Device());
    std::string reason;

    if (!device->make_instance(window, &reason) || !device->pick(&reason) || !device->make_device(&reason)) {
        if (error != nullptr) {
            *error = "Vulkan: " + reason;
        }

        return nullptr;
    }

    return device;
}

Device::~Device() {
    if (_device != VK_NULL_HANDLE) {
        _api.DeviceWaitIdle(_device);

        if (_pool != VK_NULL_HANDLE) {
            _api.DestroyCommandPool(_device, _pool, nullptr);
        }

        if (_timeline != VK_NULL_HANDLE) {
            _api.DestroySemaphore(_device, _timeline, nullptr);
        }

        _api.DestroyDevice(_device, nullptr);
    }

    if (_surface != VK_NULL_HANDLE) {
        _api.DestroySurfaceKHR(_instance, _surface, nullptr);
    }

    if (_messenger != VK_NULL_HANDLE) {
        _api.DestroyDebugUtilsMessengerEXT(_instance, _messenger, nullptr);
    }

    if (_instance != VK_NULL_HANDLE) {
        _api.DestroyInstance(_instance, nullptr);
    }

    if (_loaded) {
        SDL_Vulkan_UnloadLibrary();
    }
}

bool Device::make_instance(SDL_Window *window, std::string *error) {
    // SDL counts loads, so this is safe after SDL_WINDOW_VULKAN loaded it.
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        *error = std::string("no Vulkan loader: ") + SDL_GetError();

        return false;
    }

    _loaded = true;

    const auto lookup = as<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());

    if (lookup == nullptr) {
        *error = "no vkGetInstanceProcAddr";

        return false;
    }

#define TIV_VK_LOAD(name) _api.name = as<PFN_vk##name>(lookup(VK_NULL_HANDLE, "vk" #name));
    TIV_VK_GLOBAL(TIV_VK_LOAD)
#undef TIV_VK_LOAD

    if (_api.CreateInstance == nullptr || _api.EnumerateInstanceVersion == nullptr) {
        *error = "the loader is older than Vulkan 1.1";

        return false;
    }

    std::uint32_t count = 0;
    _api.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    _api.EnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    Uint32 sdlCount = 0;
    const char *const *sdlNames = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    std::vector<const char *> extensions;

    if (sdlNames != nullptr) {
        const std::span<const char *const> names(sdlNames, sdlCount);
        extensions.assign(names.begin(), names.end());
    }

    if (listed(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        _colourSpaces = true;
    }

    std::vector<const char *> layers;
    const char *validation = SDL_getenv("TIV_VALIDATION");

    if (validation != nullptr && *validation != '\0' && *validation != '0') {
        _api.EnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> present(count);
        _api.EnumerateInstanceLayerProperties(&count, present.data());

        if (listed(present, VALIDATION_LAYER) && listed(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(VALIDATION_LAYER.data());
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::println(stderr, "vulkan: TIV_VALIDATION is set, but the validation layer is not installed");
        }
    }

    const VkApplicationInfo application{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "tinyiv",
            .applicationVersion = 1,
            .pEngineName = "tinyiv",
            .engineVersion = 1,
            .apiVersion = VK_API_VERSION_1_2,
    };
    const VkInstanceCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application,
            .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
    };

    if (const VkResult result = _api.CreateInstance(&info, nullptr, &_instance); result != VK_SUCCESS) {
        *error = failed("vkCreateInstance", result);

        return false;
    }

#define TIV_VK_LOAD(name) _api.name = as<PFN_vk##name>(lookup(_instance, "vk" #name));
    TIV_VK_INSTANCE(TIV_VK_LOAD)
    TIV_VK_INSTANCE_OPTIONAL(TIV_VK_LOAD)
#undef TIV_VK_LOAD

    if (!layers.empty() && _api.CreateDebugUtilsMessengerEXT != nullptr) {
        const VkDebugUtilsMessengerCreateInfoEXT messenger{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity =
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = static_cast<VkDebugUtilsMessageTypeFlagsEXT>(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = report,
        };

        _api.CreateDebugUtilsMessengerEXT(_instance, &messenger, nullptr, &_messenger);
    }

    if (window != nullptr && !SDL_Vulkan_CreateSurface(window, _instance, nullptr, &_surface)) {
        *error = std::string("no Vulkan surface: ") + SDL_GetError();

        return false;
    }

    return true;
}

bool Device::pick(std::string *error) {
    std::uint32_t count = 0;
    _api.EnumeratePhysicalDevices(_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> all(count);
    _api.EnumeratePhysicalDevices(_instance, &count, all.data());

    int best = -1;

    for (VkPhysicalDevice candidate : all) {
        VkPhysicalDeviceProperties properties{};
        _api.GetPhysicalDeviceProperties(candidate, &properties);

        if (properties.apiVersion < VK_API_VERSION_1_2) {
            continue;
        }

        std::uint32_t families = 0;
        _api.GetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        _api.GetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());

        for (std::uint32_t family = 0; family < families; ++family) {
            if ((queues.at(family).queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }

            VkBool32 presents = VK_TRUE;

            if (_surface != VK_NULL_HANDLE) {
                _api.GetPhysicalDeviceSurfaceSupportKHR(candidate, family, _surface, &presents);
            }

            if (presents == VK_FALSE) {
                continue;
            }

            const int score = score_of(properties.deviceType);

            if (score > best) {
                best = score;
                _physical = candidate;
                _family = family;
                _limits = properties.limits;
                _name = static_cast<const char *>(properties.deviceName);
            }

            break;
        }
    }

    if (_physical == VK_NULL_HANDLE) {
        *error = "no device of Vulkan 1.2 or later draws to this window";

        return false;
    }

    _api.GetPhysicalDeviceMemoryProperties(_physical, &_memory);

    return true;
}

bool Device::make_device(std::string *error) {
    std::uint32_t count = 0;
    _api.EnumerateDeviceExtensionProperties(_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    _api.EnumerateDeviceExtensionProperties(_physical, nullptr, &count, available.data());

    std::vector<const char *> extensions;

    if (_surface != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        if (listed(available, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            _wantsHdrMetadata = true;
        }
    }

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = _family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan12Features features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .timelineSemaphore = VK_TRUE,
    };
    const VkDeviceCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
    };

    if (const VkResult result = _api.CreateDevice(_physical, &info, nullptr, &_device); result != VK_SUCCESS) {
        *error = failed("vkCreateDevice", result);

        return false;
    }

    const PFN_vkGetDeviceProcAddr lookup = _api.GetDeviceProcAddr;

#define TIV_VK_LOAD(name) _api.name = as<PFN_vk##name>(lookup(_device, "vk" #name));
    TIV_VK_DEVICE(TIV_VK_LOAD)

    if (_surface != VK_NULL_HANDLE) {
        TIV_VK_SWAPCHAIN(TIV_VK_LOAD)
    }

    if (_wantsHdrMetadata) {
        TIV_VK_DEVICE_OPTIONAL(TIV_VK_LOAD)
    }
#undef TIV_VK_LOAD

    _api.GetDeviceQueue(_device, _family, 0, &_queue);

    const VkSemaphoreTypeCreateInfo type{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo timeline{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    const VkCommandPoolCreateInfo pool{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = _family,
    };

    if (const VkResult result = _api.CreateSemaphore(_device, &timeline, nullptr, &_timeline); result != VK_SUCCESS) {
        *error = failed("vkCreateSemaphore", result);

        return false;
    }

    if (const VkResult result = _api.CreateCommandPool(_device, &pool, nullptr, &_pool); result != VK_SUCCESS) {
        *error = failed("vkCreateCommandPool", result);

        return false;
    }

    return true;
}

VkCommandBuffer Device::begin() {
    const std::uint64_t done = completed();
    auto free = std::ranges::find_if(_recorders, [&](const Recorder &each) { return each.serial <= done; });

    if (free == _recorders.end()) {
        Recorder made;
        const VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = _pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
        };

        if (!check(_api.AllocateCommandBuffers(_device, &info, &made.commands))) {
            return VK_NULL_HANDLE;
        }

        _recorders.push_back(made);
        free = _recorders.end() - 1;
    }

    // Marked as taken until submit() gives it a serial.
    free->serial = ~std::uint64_t{0};
    _api.ResetCommandBuffer(free->commands, 0);

    const VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    _api.BeginCommandBuffer(free->commands, &info);

    return free->commands;
}

std::uint64_t
Device::submit(VkCommandBuffer commands, VkSemaphore wait, const VkPipelineStageFlags waitStage, VkSemaphore signal) {
    const std::uint64_t serial = _submitted + 1;
    const std::array<VkSemaphore, 2> signals{_timeline, signal};
    const std::array<std::uint64_t, 2> values{serial, 0};
    const VkTimelineSemaphoreSubmitInfo timeline{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = signal != VK_NULL_HANDLE ? 2U : 1U,
            .pSignalSemaphoreValues = values.data(),
    };
    const VkSubmitInfo info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline,
            .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1U : 0U,
            .pWaitSemaphores = &wait,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &commands,
            .signalSemaphoreCount = signal != VK_NULL_HANDLE ? 2U : 1U,
            .pSignalSemaphores = signals.data(),
    };

    _api.EndCommandBuffer(commands);

    for (Recorder &each : _recorders) {
        if (each.commands == commands) {
            each.serial = serial;
        }
    }

    if (!check(_api.QueueSubmit(_queue, 1, &info, VK_NULL_HANDLE))) {
        // A submit fails only out of memory or with the device gone, and drawing stops either way.
        _lost = true;

        for (Recorder &each : _recorders) {
            if (each.commands == commands) {
                each.serial = 0;
            }
        }

        return _submitted;
    }

    _submitted = serial;

    return serial;
}

std::uint64_t Device::completed() {
    if (_completed < _submitted) {
        std::uint64_t value = 0;

        if (check(_api.GetSemaphoreCounterValue(_device, _timeline, &value))) {
            _completed = value;
        }
    }

    return _completed;
}

void Device::wait(const std::uint64_t serial) {
    if (serial <= _completed || _lost) {
        return;
    }

    const VkSemaphoreWaitInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &_timeline,
            .pValues = &serial,
    };

    if (check(_api.WaitSemaphores(_device, &info, ~std::uint64_t{0}))) {
        _completed = std::max(_completed, serial);
    }
}

std::uint32_t Device::memory_type(const std::uint32_t bits,
                                  const VkMemoryPropertyFlags wanted,
                                  const VkMemoryPropertyFlags avoided) const {
    const std::span types(static_cast<const VkMemoryType *>(_memory.memoryTypes), _memory.memoryTypeCount);
    std::uint32_t fallback = NO_MEMORY;

    for (std::uint32_t index = 0; index < types.size(); ++index) {
        const VkMemoryPropertyFlags flags = types[index].propertyFlags;

        if ((bits & (1U << index)) == 0 || (flags & wanted) != wanted) {
            continue;
        }

        if ((flags & avoided) == 0) {
            return index;
        }

        fallback = std::min(fallback, index);
    }

    return fallback;
}

bool Device::check(const VkResult result) {
    if (result == VK_ERROR_DEVICE_LOST) {
        _lost = true;
    }

    return result >= 0;
}

int Device::validation_messages() {
    return messages().load();
}
}
