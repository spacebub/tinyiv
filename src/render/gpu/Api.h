// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_RENDER_GPU_API_H
#define TIV_RENDER_GPU_API_H


#include <vulkan/vulkan.h>

// No Vulkan library is linked. Entry points come through the loader SDL opens.
#ifndef VK_NO_PROTOTYPES
#error "build with VK_NO_PROTOTYPES"
#endif

namespace tiv::gpu {

// NOLINTBEGIN(cppcoreguidelines-macro-usage): one list both declares and loads the entry points.
#define TIV_VK_GLOBAL(X)                                                                                               \
    X(CreateInstance)                                                                                                  \
    X(EnumerateInstanceExtensionProperties)                                                                            \
    X(EnumerateInstanceLayerProperties)                                                                                \
    X(EnumerateInstanceVersion)

#define TIV_VK_INSTANCE(X)                                                                                             \
    X(DestroyInstance)                                                                                                 \
    X(EnumeratePhysicalDevices)                                                                                        \
    X(GetPhysicalDeviceProperties)                                                                                     \
    X(GetPhysicalDeviceQueueFamilyProperties)                                                                          \
    X(GetPhysicalDeviceMemoryProperties)                                                                               \
    X(EnumerateDeviceExtensionProperties)                                                                              \
    X(CreateDevice)                                                                                                    \
    X(GetDeviceProcAddr)                                                                                               \
    X(DestroySurfaceKHR)                                                                                               \
    X(GetPhysicalDeviceSurfaceSupportKHR)                                                                              \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                         \
    X(GetPhysicalDeviceSurfaceFormatsKHR)                                                                              \
    X(GetPhysicalDeviceSurfacePresentModesKHR)

#define TIV_VK_INSTANCE_OPTIONAL(X)                                                                                    \
    X(CreateDebugUtilsMessengerEXT)                                                                                    \
    X(DestroyDebugUtilsMessengerEXT)

#define TIV_VK_DEVICE(X)                                                                                               \
    X(DestroyDevice)                                                                                                   \
    X(GetDeviceQueue)                                                                                                  \
    X(DeviceWaitIdle)                                                                                                  \
    X(QueueSubmit)                                                                                                     \
    X(CreateImage)                                                                                                     \
    X(DestroyImage)                                                                                                    \
    X(CreateImageView)                                                                                                 \
    X(DestroyImageView)                                                                                                \
    X(GetImageMemoryRequirements)                                                                                      \
    X(BindImageMemory)                                                                                                 \
    X(CreateBuffer)                                                                                                    \
    X(DestroyBuffer)                                                                                                   \
    X(GetBufferMemoryRequirements)                                                                                     \
    X(BindBufferMemory)                                                                                                \
    X(AllocateMemory)                                                                                                  \
    X(FreeMemory)                                                                                                      \
    X(MapMemory)                                                                                                       \
    X(CreateSampler)                                                                                                   \
    X(DestroySampler)                                                                                                  \
    X(CreateDescriptorSetLayout)                                                                                       \
    X(DestroyDescriptorSetLayout)                                                                                      \
    X(CreateDescriptorPool)                                                                                            \
    X(DestroyDescriptorPool)                                                                                           \
    X(AllocateDescriptorSets)                                                                                          \
    X(FreeDescriptorSets)                                                                                              \
    X(UpdateDescriptorSets)                                                                                            \
    X(CreatePipelineLayout)                                                                                            \
    X(DestroyPipelineLayout)                                                                                           \
    X(CreateShaderModule)                                                                                              \
    X(DestroyShaderModule)                                                                                             \
    X(CreateGraphicsPipelines)                                                                                         \
    X(DestroyPipeline)                                                                                                 \
    X(CreateRenderPass)                                                                                                \
    X(DestroyRenderPass)                                                                                               \
    X(CreateFramebuffer)                                                                                               \
    X(DestroyFramebuffer)                                                                                              \
    X(CreateCommandPool)                                                                                               \
    X(DestroyCommandPool)                                                                                              \
    X(AllocateCommandBuffers)                                                                                          \
    X(BeginCommandBuffer)                                                                                              \
    X(EndCommandBuffer)                                                                                                \
    X(ResetCommandBuffer)                                                                                              \
    X(CreateSemaphore)                                                                                                 \
    X(DestroySemaphore)                                                                                                \
    X(GetSemaphoreCounterValue)                                                                                        \
    X(WaitSemaphores)                                                                                                  \
    X(CmdPipelineBarrier)                                                                                              \
    X(CmdCopyBufferToImage)                                                                                            \
    X(CmdCopyImageToBuffer)                                                                                            \
    X(CmdBeginRenderPass)                                                                                              \
    X(CmdEndRenderPass)                                                                                                \
    X(CmdBindPipeline)                                                                                                 \
    X(CmdBindDescriptorSets)                                                                                           \
    X(CmdBindVertexBuffers)                                                                                            \
    X(CmdPushConstants)                                                                                                \
    X(CmdSetViewport)                                                                                                  \
    X(CmdSetScissor)                                                                                                   \
    X(CmdDraw)

#define TIV_VK_SWAPCHAIN(X)                                                                                            \
    X(CreateSwapchainKHR)                                                                                              \
    X(DestroySwapchainKHR)                                                                                             \
    X(GetSwapchainImagesKHR)                                                                                           \
    X(AcquireNextImageKHR)                                                                                             \
    X(QueuePresentKHR)

#define TIV_VK_DEVICE_OPTIONAL(X) X(SetHdrMetadataEXT)

#define TIV_VK_MEMBER(name) PFN_vk##name name = nullptr;
// NOLINTEND(cppcoreguidelines-macro-usage)

struct Api {
    TIV_VK_GLOBAL(TIV_VK_MEMBER)
    TIV_VK_INSTANCE(TIV_VK_MEMBER)
    TIV_VK_INSTANCE_OPTIONAL(TIV_VK_MEMBER)
    TIV_VK_DEVICE(TIV_VK_MEMBER)
    TIV_VK_SWAPCHAIN(TIV_VK_MEMBER)
    TIV_VK_DEVICE_OPTIONAL(TIV_VK_MEMBER)
};

}


#endif //TIV_RENDER_GPU_API_H
