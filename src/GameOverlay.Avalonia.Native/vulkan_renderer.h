#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "shared_state.h"

namespace overlay {

// Composites the overlay onto a Vulkan game's swapchain image.
//
// Vulkan is the most invasive of the three backends:
//
//   * It does not go through DXGI, so none of the Present hooks apply -
//     vkQueuePresentKHR is the entry point.
//   * Nothing can be recovered after the fact. The device, queue and swapchain
//     images all have to be captured as the game creates them, which is why
//     the payload must be loaded before the game starts.
//   * Importing the host's texture requires VK_KHR_external_memory_win32 to
//     have been enabled at vkCreateDevice time, so the hook rewrites the
//     game's extension list on the way through.
//
// Synchronisation reuses the D3D11 keyed mutex unchanged:
// VK_KHR_win32_keyed_mutex can acquire the same mutex the host releases, so
// unlike D3D12 no separate fence protocol is needed.
class VulkanRenderer {
public:
    // --- called from the hooks as the game builds its objects -------------
    void OnDeviceCreated(VkPhysicalDevice physical, VkDevice device, HMODULE loader,
                         SharedState* state);
    void OnQueue(VkQueue queue, uint32_t queueFamily);
    void OnSwapchainCreated(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent);
    void OnSwapchainDestroyed(VkSwapchainKHR swapchain);

    // Records and submits the overlay draw for the image about to be presented.
    // Returns the semaphore present must now wait on, or VK_NULL_HANDLE to
    // leave the game's present untouched.
    VkSemaphore Render(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex,
                       const VkSemaphore* waitSemaphores, uint32_t waitCount,
                       SharedState* state);

    void Shutdown();

    bool ready() const { return device_ != VK_NULL_HANDLE && queue_ != VK_NULL_HANDLE; }

private:
    struct PerImage {
        VkImageView     view = VK_NULL_HANDLE;
        VkFramebuffer   framebuffer = VK_NULL_HANDLE;
        VkCommandBuffer draw = VK_NULL_HANDLE;
        VkCommandBuffer copy = VK_NULL_HANDLE;
        VkSemaphore     finished = VK_NULL_HANDLE;
        VkFence         inFlight = VK_NULL_HANDLE;
    };

    bool LoadFunctions(HMODULE loader);
    void PublishAdapterLuid(SharedState* state, HMODULE loader);
    bool CreatePipeline();
    bool EnsureSwapchainResources(VkSwapchainKHR swapchain);
    bool EnsureSharedTexture(SharedState* state);
    bool CopySharedToPrivate(VkQueue queue, PerImage& image);
    void ReleaseSwapchainResources();
    void ReleaseSharedTexture();
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;

    VkInstance       instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    uint32_t         queueFamily_ = 0;

    VkSwapchainKHR   swapchain_ = VK_NULL_HANDLE;
    VkFormat         swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D       swapchainExtent_{};
    std::vector<PerImage> images_;

    VkRenderPass          renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;
    VkCommandPool         commandPool_ = VK_NULL_HANDLE;
    VkSampler             sampler_ = VK_NULL_HANDLE;

    // The host's texture, imported. Copied into a private image while holding
    // the keyed mutex so the overlay draw itself never needs it - which keeps a
    // busy mutex from ever breaking the present dependency chain.
    VkImage        sharedImage_ = VK_NULL_HANDLE;
    VkDeviceMemory sharedMemory_ = VK_NULL_HANDLE;
    VkImage        privateImage_ = VK_NULL_HANDLE;
    VkDeviceMemory privateMemory_ = VK_NULL_HANDLE;
    VkImageView    privateView_ = VK_NULL_HANDLE;
    bool           privateHasContent_ = false;
    uint64_t       openedHandle_ = 0;
    uint32_t       textureWidth_ = 0;
    uint32_t       textureHeight_ = 0;
    uint32_t       lastFrameIndex_ = 0;

    bool functionsLoaded_ = false;
    bool pipelineReady_ = false;
    bool failed_ = false;
};

} // namespace overlay
