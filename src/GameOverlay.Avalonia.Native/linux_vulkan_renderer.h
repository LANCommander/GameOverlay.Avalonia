// linux_vulkan_renderer.h - composites the overlay onto a Linux Vulkan game's
// swapchain image, driven from the overlay Vulkan layer's vkQueuePresentKHR.
//
// The Linux counterpart of vulkan_renderer.* but CPU-sourced: the overlay pixels
// arrive through the same POSIX-shm CPU frame transport the GLX and D3D9 paths
// use, so there is no external-memory import and no keyed mutex. Each present it
// uploads the latest frame into a sampled image and records a draw over the
// swapchain image, then hands present a semaphore to wait on in place of the
// game's (mirroring the Windows QueuePresentHook).
#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "shared_state.h"

namespace overlay {

class LinuxVulkanRenderer {
public:
    // Called by the layer as the game builds its objects. importEnabled is true
    // when the device enabled VK_EXT_external_memory_host, allowing the overlay
    // frame buffer to be imported directly (zero-copy) rather than uploaded.
    void Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
              PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, bool importEnabled);
    void OnQueue(VkQueue queue, uint32_t queueFamily);
    void OnSwapchainCreated(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent);
    void OnSwapchainDestroyed(VkSwapchainKHR swapchain);

    // Records and submits the overlay draw for the image about to be presented.
    // Returns the semaphore present must now wait on, or VK_NULL_HANDLE to leave
    // the game's present untouched.
    VkSemaphore Render(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex,
                       const VkSemaphore* waitSemaphores, uint32_t waitCount, SharedState* state);

    void Shutdown();

private:
    struct PerImage {
        VkImage         image = VK_NULL_HANDLE;   // swapchain image (not owned)
        VkImageView     view = VK_NULL_HANDLE;
        VkFramebuffer   framebuffer = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore     finished = VK_NULL_HANDLE;
        VkFence         inFlight = VK_NULL_HANDLE;
        bool            fenceSubmitted = false;
    };

    bool CreatePipeline();
    bool EnsureSwapchainResources(VkSwapchainKHR swapchain);
    bool EnsureOverlayImage(uint32_t width, uint32_t height);
    void ReleaseSwapchainResources();
    void ReleaseOverlayImage();
    bool EnsureFrameMapping(SharedState* state);
    bool ReadFrameIntoStaging(SharedState* state);
    // Zero-copy path: import the mapped frame buffer directly as a sampled image.
    bool EnsureImportedImage(SharedState* state);
    void ReleaseImportedImage();
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;

    // Optional one-shot capture (OVERLAY_VK_CAPTURE=<ppm>): after compositing on
    // a chosen frame, copy the swapchain image back and count pixels that differ
    // from the corner - the Vulkan analogue of the GLX sample's coveredPixels.
    bool EnsureCaptureBuffer();
    void DoCaptureReadback(VkCommandBuffer cmd, const PerImage& img);
    void FinishCapture();

    VkInstance       instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    uint32_t         queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};

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

    // Overlay texture (CPU-uploaded) + host-visible staging.
    VkImage        overlayImage_ = VK_NULL_HANDLE;
    VkDeviceMemory overlayMemory_ = VK_NULL_HANDLE;
    VkImageView    overlayView_ = VK_NULL_HANDLE;
    VkBuffer       staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void*          stagingMapped_ = nullptr;
    uint32_t       texWidth_ = 0;
    uint32_t       texHeight_ = 0;
    bool           overlayInitialized_ = false;   // has valid layout/content

    // CPU frame shm (mirrors opengl_glx_renderer).
    int            frameFd_ = -1;
    const uint8_t* framePixels_ = nullptr;
    std::size_t    frameBytes_ = 0;         // actual pixel bytes (w*h*4)
    std::size_t    frameMappedBytes_ = 0;   // page-rounded mmap size (for import)
    uint32_t       openedGeneration_ = 0;

    // Zero-copy import (VK_EXT_external_memory_host): the mapped frame buffer is
    // bound directly as image memory instead of being uploaded each present.
    bool           importEnabled_ = false;   // device enabled the extension
    bool           useImport_ = false;        // import chosen at runtime
    bool           importDecided_ = false;
    bool           importLayoutSet_ = false;
    VkImage        importImage_ = VK_NULL_HANDLE;
    VkDeviceMemory importMemory_ = VK_NULL_HANDLE;
    VkImageView    importView_ = VK_NULL_HANDLE;
    uint32_t       importGeneration_ = 0;

    bool functionsLoaded_ = false;
    bool pipelineReady_ = false;
    bool failed_ = false;
    bool hasContent_ = false;

    // Capture diagnostic state.
    const char*    capturePath_ = nullptr;
    uint32_t       captureAtDraw_ = 120;
    bool           captureChecked_ = false;
    bool           captured_ = false;
    bool           capturePending_ = false;
    VkBuffer       captureBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory captureMemory_ = VK_NULL_HANDLE;
    void*          captureMapped_ = nullptr;
    VkDeviceSize   captureBytes_ = 0;
    uint32_t       drawCount_ = 0;
};

} // namespace overlay
