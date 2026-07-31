// Device- and instance-level Vulkan entry points the compositor needs.
//
// Resolved through the game's own vkGetInstanceProcAddr / vkGetDeviceProcAddr
// rather than linked, so the payload has no import dependency on vulkan-1.dll
// and works with whatever loader the game already has.
#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace overlay {

#define OVERLAY_VK_DEVICE_FUNCTIONS(X)                                        \
    X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkGetSwapchainImagesKHR)           \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)        \
    X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory)                  \
    X(vkCreateImageView) X(vkDestroyImageView)                                \
    X(vkCreateSampler) X(vkDestroySampler)                                    \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)            \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)                      \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                     \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                      \
    X(vkCreateShaderModule) X(vkDestroyShaderModule)                          \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                         \
    X(vkCreateRenderPass) X(vkDestroyRenderPass)                              \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer)                            \
    X(vkCreateCommandPool) X(vkDestroyCommandPool)                            \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)                       \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)     \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)                             \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)                           \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)                       \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImage)                                 \
    X(vkCreateSemaphore) X(vkDestroySemaphore)                                \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)                     \
    X(vkResetFences) X(vkGetFenceStatus)

struct VulkanFunctions {
#define OVERLAY_VK_DECLARE(name) PFN_##name name = nullptr;
    OVERLAY_VK_DEVICE_FUNCTIONS(OVERLAY_VK_DECLARE)
#undef OVERLAY_VK_DECLARE

    // Instance-level.
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;

    // From VK_KHR_external_memory_win32. Reports which memory types an imported
    // handle may be bound to; guessing instead is a portability trap.
    PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
};

} // namespace overlay
