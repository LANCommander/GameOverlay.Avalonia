// SampleGameVulkan - the Vulkan counterpart of the D3D sample games.
//
// Vulkan is the hardest target for an overlay: it does not go through DXGI, so
// none of the Present hooks apply, and the objects an overlay needs (device,
// queue, swapchain images) cannot be recovered after the fact the way D3D's
// can. This target exists to exercise that path against something we control.
//
// Controls:
//   F1  windowed          F2  borderless fullscreen     F3  exclusive fullscreen
//   V   toggle vsync      C   clear frame stats         ESC quit

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "triangle_shaders.h"

namespace {

enum class DisplayMode { Windowed, Borderless, Exclusive };

constexpr uint32_t kMaxFramesInFlight = 2;

HWND             g_hwnd = nullptr;
HMODULE          g_vulkan = nullptr;

VkInstance       g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
VkDevice         g_device = VK_NULL_HANDLE;
VkQueue          g_queue = VK_NULL_HANDLE;
uint32_t         g_queueFamily = 0;
VkSurfaceKHR     g_surface = VK_NULL_HANDLE;
VkSwapchainKHR   g_swapchain = VK_NULL_HANDLE;
VkFormat         g_swapchainFormat = VK_FORMAT_UNDEFINED;
VkExtent2D       g_swapchainExtent{};

std::vector<VkImage>       g_images;
std::vector<VkImageView>   g_imageViews;
std::vector<VkFramebuffer> g_framebuffers;

VkRenderPass     g_renderPass = VK_NULL_HANDLE;
VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkPipeline       g_pipeline = VK_NULL_HANDLE;
VkCommandPool    g_commandPool = VK_NULL_HANDLE;

std::vector<VkCommandBuffer> g_commandBuffers;
VkSemaphore  g_imageAvailable[kMaxFramesInFlight]{};
VkSemaphore  g_renderFinished[kMaxFramesInFlight]{};
VkFence      g_inFlight[kMaxFramesInFlight]{};
uint32_t     g_frame = 0;

bool         g_vsync = false;
bool         g_exclusiveSupported = false;
bool         g_exclusiveAcquired = false;
DisplayMode  g_mode = DisplayMode::Windowed;
bool         g_running = true;
bool         g_inSizeMove = false;
bool         g_exclusiveRefused = false;
bool         g_needsRecreate = false;
RECT         g_windowedRect = { 0, 0, 1280, 720 };
unsigned     g_inputReceived = 0;

std::vector<double> g_frameTimes;
LARGE_INTEGER       g_qpcFreq{};
LARGE_INTEGER       g_lastFrameQpc{};
double              g_titleAccumMs = 0.0;

// --- loader ---------------------------------------------------------------
// Resolved from vulkan-1.dll rather than linked, so no Vulkan SDK is needed to
// build this.
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_ = nullptr;
#define VK_FN(name) PFN_##name name##_ = nullptr
VK_FN(vkCreateInstance); VK_FN(vkDestroyInstance);
VK_FN(vkEnumeratePhysicalDevices); VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
VK_FN(vkCreateDevice); VK_FN(vkDestroyDevice); VK_FN(vkGetDeviceQueue);
VK_FN(vkGetDeviceProcAddr); VK_FN(vkDeviceWaitIdle);
VK_FN(vkCreateWin32SurfaceKHR); VK_FN(vkDestroySurfaceKHR);
VK_FN(vkGetPhysicalDeviceSurfaceSupportKHR); VK_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
VK_FN(vkGetPhysicalDeviceSurfaceFormatsKHR); VK_FN(vkGetPhysicalDeviceSurfacePresentModesKHR);
VK_FN(vkCreateSwapchainKHR); VK_FN(vkDestroySwapchainKHR); VK_FN(vkGetSwapchainImagesKHR);
VK_FN(vkAcquireNextImageKHR); VK_FN(vkQueuePresentKHR); VK_FN(vkQueueSubmit); VK_FN(vkQueueWaitIdle);
VK_FN(vkCreateImageView); VK_FN(vkDestroyImageView);
VK_FN(vkCreateRenderPass); VK_FN(vkDestroyRenderPass);
VK_FN(vkCreateFramebuffer); VK_FN(vkDestroyFramebuffer);
VK_FN(vkCreateShaderModule); VK_FN(vkDestroyShaderModule);
VK_FN(vkCreatePipelineLayout); VK_FN(vkDestroyPipelineLayout);
VK_FN(vkCreateGraphicsPipelines); VK_FN(vkDestroyPipeline);
VK_FN(vkCreateCommandPool); VK_FN(vkDestroyCommandPool);
VK_FN(vkAllocateCommandBuffers); VK_FN(vkFreeCommandBuffers);
VK_FN(vkBeginCommandBuffer); VK_FN(vkEndCommandBuffer); VK_FN(vkResetCommandBuffer);
VK_FN(vkCmdBeginRenderPass); VK_FN(vkCmdEndRenderPass); VK_FN(vkCmdBindPipeline);
VK_FN(vkCmdSetViewport); VK_FN(vkCmdSetScissor); VK_FN(vkCmdPushConstants); VK_FN(vkCmdDraw);
VK_FN(vkCreateSemaphore); VK_FN(vkDestroySemaphore);
VK_FN(vkCreateFence); VK_FN(vkDestroyFence); VK_FN(vkWaitForFences); VK_FN(vkResetFences);
VK_FN(vkAcquireFullScreenExclusiveModeEXT); VK_FN(vkReleaseFullScreenExclusiveModeEXT);
#undef VK_FN

void Fatal(const char* what, VkResult result = VK_SUCCESS) {
    char msg[512];
    sprintf_s(msg, "%s failed (VkResult %d)", what, static_cast<int>(result));
    MessageBoxA(nullptr, msg, "SampleGameVulkan", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) Fatal(what, result);
}

void LoadLoader() {
    g_vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vulkan) Fatal("LoadLibrary(vulkan-1.dll)");

    vkGetInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkan, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr_) Fatal("GetProcAddress(vkGetInstanceProcAddr)");

    vkCreateInstance_ = reinterpret_cast<PFN_vkCreateInstance>(
        vkGetInstanceProcAddr_(nullptr, "vkCreateInstance"));
}

void LoadInstanceFunctions() {
#define LOAD(name) name##_ = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr_(g_instance, #name))
    LOAD(vkDestroyInstance); LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice); LOAD(vkDestroyDevice); LOAD(vkGetDeviceQueue);
    LOAD(vkGetDeviceProcAddr); LOAD(vkDeviceWaitIdle);
    LOAD(vkCreateWin32SurfaceKHR); LOAD(vkDestroySurfaceKHR);
    LOAD(vkGetPhysicalDeviceSurfaceSupportKHR); LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR); LOAD(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD(vkCreateSwapchainKHR); LOAD(vkDestroySwapchainKHR); LOAD(vkGetSwapchainImagesKHR);
    LOAD(vkAcquireNextImageKHR); LOAD(vkQueuePresentKHR); LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle);
    LOAD(vkCreateImageView); LOAD(vkDestroyImageView);
    LOAD(vkCreateRenderPass); LOAD(vkDestroyRenderPass);
    LOAD(vkCreateFramebuffer); LOAD(vkDestroyFramebuffer);
    LOAD(vkCreateShaderModule); LOAD(vkDestroyShaderModule);
    LOAD(vkCreatePipelineLayout); LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateGraphicsPipelines); LOAD(vkDestroyPipeline);
    LOAD(vkCreateCommandPool); LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers); LOAD(vkFreeCommandBuffers);
    LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkResetCommandBuffer);
    LOAD(vkCmdBeginRenderPass); LOAD(vkCmdEndRenderPass); LOAD(vkCmdBindPipeline);
    LOAD(vkCmdSetViewport); LOAD(vkCmdSetScissor); LOAD(vkCmdPushConstants); LOAD(vkCmdDraw);
    LOAD(vkCreateSemaphore); LOAD(vkDestroySemaphore);
    LOAD(vkCreateFence); LOAD(vkDestroyFence); LOAD(vkWaitForFences); LOAD(vkResetFences);
    LOAD(vkAcquireFullScreenExclusiveModeEXT); LOAD(vkReleaseFullScreenExclusiveModeEXT);
#undef LOAD
}

void CreateInstance() {
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "SampleGameVulkan";
    app.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        // Required by VK_EXT_full_screen_exclusive, which the device enables.
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    info.ppEnabledExtensionNames = extensions;

    Check(vkCreateInstance_(&info, nullptr, &g_instance), "vkCreateInstance");
    LoadInstanceFunctions();
}

void PickPhysicalDeviceAndQueue() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices_(g_instance, &count, nullptr);
    if (count == 0) Fatal("no Vulkan physical devices");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices_(g_instance, &count, devices.data());

    for (VkPhysicalDevice candidate : devices) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties_(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties_(candidate, &families, props.data());

        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR_(candidate, i, g_surface, &present);
            if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                g_physicalDevice = candidate;
                g_queueFamily = i;
                return;
            }
        }
    }
    Fatal("no graphics+present queue family");
}

void CreateDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = g_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Only the swapchain extension is requested here. The overlay adds the
    // external-memory ones by rewriting this list inside its vkCreateDevice
    // hook - which is exactly why it has to be loaded before the game starts.
    std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (g_exclusiveSupported) extensions.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);

    VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    Check(vkCreateDevice_(g_physicalDevice, &info, nullptr, &g_device), "vkCreateDevice");
    vkGetDeviceQueue_(g_device, g_queueFamily, 0, &g_queue);
}

VkShaderModule CreateShader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = bytes;
    info.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule_(g_device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void CreateRenderPass() {
    VkAttachmentDescription colour{};
    colour.format = g_swapchainFormat;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The overlay will render into this image afterwards, so leave it in
    // PRESENT_SRC - that is the layout it expects to transition from.
    colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    info.attachmentCount = 1;
    info.pAttachments = &colour;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;

    Check(vkCreateRenderPass_(g_device, &info, nullptr, &g_renderPass), "vkCreateRenderPass");
}

void CreatePipeline() {
    VkShaderModule vs = CreateShader(kTriangleVertSpv, sizeof(kTriangleVertSpv));
    VkShaderModule fs = CreateShader(kTriangleFragSpv, sizeof(kTriangleFragSpv));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Same reason as the D3D targets: the generated winding would otherwise be
    // culled at every angle.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout_(g_device, &layoutInfo, nullptr, &g_pipelineLayout),
          "vkCreatePipelineLayout");

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = g_pipelineLayout;
    info.renderPass = g_renderPass;

    Check(vkCreateGraphicsPipelines_(g_device, VK_NULL_HANDLE, 1, &info, nullptr, &g_pipeline),
          "vkCreateGraphicsPipelines");

    vkDestroyShaderModule_(g_device, vs, nullptr);
    vkDestroyShaderModule_(g_device, fs, nullptr);
}

void DestroySwapchainResources() {
    for (VkFramebuffer fb : g_framebuffers) vkDestroyFramebuffer_(g_device, fb, nullptr);
    for (VkImageView view : g_imageViews) vkDestroyImageView_(g_device, view, nullptr);
    g_framebuffers.clear();
    g_imageViews.clear();
    g_images.clear();
}

void CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR_(g_physicalDevice, g_surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilities");

    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return;

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR_(g_physicalDevice, g_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR_(g_physicalDevice, g_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    // IMMEDIATE when vsync is off, so the overlay's per-frame cost is not
    // hidden inside a wait for the display refresh.
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR_(g_physicalDevice, g_surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR_(g_physicalDevice, g_surface, &modeCount, modes.data());
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { present = m; break; }
        }
    }

    uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = g_surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = caps.currentExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present;
    info.clipped = VK_TRUE;

    const bool wantExclusive = g_exclusiveSupported && g_mode == DisplayMode::Exclusive &&
                               vkAcquireFullScreenExclusiveModeEXT_ != nullptr;

    // Application-controlled exclusive fullscreen on Windows needs BOTH structs
    // chained: the generic one to opt in, and the Win32 one to name the monitor.
    HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    VkSurfaceFullScreenExclusiveWin32InfoEXT exclusiveWin32{
        VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT };
    exclusiveWin32.hmonitor = monitor;

    VkSurfaceFullScreenExclusiveInfoEXT exclusive{ VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    exclusive.pNext = &exclusiveWin32;
    exclusive.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    if (wantExclusive) info.pNext = &exclusive;

    VkSwapchainKHR old = g_swapchain;
    info.oldSwapchain = old;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult swapResult = vkCreateSwapchainKHR_(g_device, &info, nullptr, &created);
    if (swapResult != VK_SUCCESS && wantExclusive) {
        // Exclusive can be refused; drop the exclusive chain and try again as
        // borderless rather than taking the game down.
        info.pNext = nullptr;
        g_exclusiveRefused = true;
        swapResult = vkCreateSwapchainKHR_(g_device, &info, nullptr, &created);
    }
    Check(swapResult, "vkCreateSwapchainKHR");

    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR_(g_device, old, nullptr);
    g_swapchain = created;
    g_swapchainFormat = chosen.format;
    g_swapchainExtent = caps.currentExtent;

    // With APPLICATION_CONTROLLED, the swapchain is created but not actually in
    // exclusive mode until this call - and it is allowed to fail (another app
    // owns the output, the window is not foreground), so treat that as a
    // graceful fall back to borderless.
    if (wantExclusive && !g_exclusiveRefused) {
        VkResult acquired = vkAcquireFullScreenExclusiveModeEXT_(g_device, g_swapchain);
        g_exclusiveRefused = (acquired != VK_SUCCESS);
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR_(g_device, g_swapchain, &count, nullptr);
    g_images.resize(count);
    vkGetSwapchainImagesKHR_(g_device, g_swapchain, &count, g_images.data());

    g_imageViews.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = g_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_swapchainFormat;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView_(g_device, &viewInfo, nullptr, &g_imageViews[i]), "vkCreateImageView");
    }
}

void CreateFramebuffers() {
    g_framebuffers.resize(g_imageViews.size());
    for (size_t i = 0; i < g_imageViews.size(); ++i) {
        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = g_renderPass;
        info.attachmentCount = 1;
        info.pAttachments = &g_imageViews[i];
        info.width = g_swapchainExtent.width;
        info.height = g_swapchainExtent.height;
        info.layers = 1;
        Check(vkCreateFramebuffer_(g_device, &info, nullptr, &g_framebuffers[i]), "vkCreateFramebuffer");
    }
}

void RecreateSwapchain() {
    vkDeviceWaitIdle_(g_device);
    DestroySwapchainResources();
    CreateSwapchain();
    if (g_images.empty()) return;
    CreateFramebuffers();
    g_needsRecreate = false;
}

void InitVulkan() {
    LoadLoader();
    CreateInstance();

    VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = g_hwnd;
    Check(vkCreateWin32SurfaceKHR_(g_instance, &surfaceInfo, nullptr, &g_surface), "vkCreateWin32SurfaceKHR");

    PickPhysicalDeviceAndQueue();

    // VK_EXT_full_screen_exclusive is how Vulkan expresses what D3D calls
    // exclusive fullscreen. It is optional, so degrade to borderless if absent.
    g_exclusiveSupported = true;

    CreateDevice();
    CreateSwapchain();
    if (g_images.empty()) Fatal("swapchain has no images");
    CreateRenderPass();
    CreateFramebuffers();
    CreatePipeline();

    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_queueFamily;
    Check(vkCreateCommandPool_(g_device, &poolInfo, nullptr, &g_commandPool), "vkCreateCommandPool");

    g_commandBuffers.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = g_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    Check(vkAllocateCommandBuffers_(g_device, &allocInfo, g_commandBuffers.data()),
          "vkAllocateCommandBuffers");

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        Check(vkCreateSemaphore_(g_device, &semInfo, nullptr, &g_imageAvailable[i]), "vkCreateSemaphore");
        Check(vkCreateSemaphore_(g_device, &semInfo, nullptr, &g_renderFinished[i]), "vkCreateSemaphore");

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        Check(vkCreateFence_(g_device, &fenceInfo, nullptr, &g_inFlight[i]), "vkCreateFence");
    }
}

void SetDisplayMode(DisplayMode mode) {
    if (mode == g_mode) return;

    if (g_mode == DisplayMode::Windowed && mode != DisplayMode::Windowed) {
        GetWindowRect(g_hwnd, &g_windowedRect);
    }

    g_exclusiveRefused = false;

    if (mode == DisplayMode::Windowed) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, g_windowedRect.left, g_windowedRect.top,
                     g_windowedRect.right - g_windowedRect.left,
                     g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED);
    } else {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }

    g_mode = mode;
    g_needsRecreate = true;
}

void UpdateTitle(double frameMs) {
    g_titleAccumMs += frameMs;
    if (g_titleAccumMs < 250.0) return;
    g_titleAccumMs = 0.0;

    size_t take = std::min<size_t>(g_frameTimes.size(), 2000);
    std::vector<double> recent(g_frameTimes.end() - static_cast<ptrdiff_t>(take), g_frameTimes.end());
    std::sort(recent.begin(), recent.end());

    double avg = 0.0;
    for (double v : recent) avg += v;
    avg = recent.empty() ? 0.0 : avg / recent.size();
    double p50 = recent.empty() ? 0.0 : recent[recent.size() / 2];
    double p99 = recent.empty() ? 0.0 : recent[static_cast<size_t>(recent.size() * 0.99)];

    const char* modeName = g_mode == DisplayMode::Windowed   ? "windowed"
                         : g_mode == DisplayMode::Borderless ? "borderless"
                                                             : "EXCLUSIVE";
    char title[380];
    sprintf_s(title,
              "SampleGameVulkan [%s]%s vsync:%s | %.0f fps | avg %.3f ms  p50 %.3f  p99 %.3f | input %u | pid %lu",
              modeName, g_exclusiveRefused ? " (exclusive REFUSED)" : "",
              g_vsync ? "on" : "off",
              avg > 0.0 ? 1000.0 / avg : 0.0, avg, p50, p99,
              g_inputReceived, GetCurrentProcessId());
    SetWindowTextA(g_hwnd, title);
}

void RenderFrame() {
    if (g_needsRecreate) { RecreateSwapchain(); if (g_images.empty()) return; }

    const uint32_t slot = g_frame % kMaxFramesInFlight;

    vkWaitForFences_(g_device, 1, &g_inFlight[slot], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquired = vkAcquireNextImageKHR_(g_device, g_swapchain, UINT64_MAX,
                                               g_imageAvailable[slot], VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        g_needsRecreate = true;
        return;
    }
    if (acquired != VK_SUCCESS) return;

    vkResetFences_(g_device, 1, &g_inFlight[slot]);

    static float angle = 0.0f;
    angle += 0.01f;

    VkCommandBuffer cmd = g_commandBuffers[slot];
    vkResetCommandBuffer_(cmd, 0);

    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer_(cmd, &begin);

    VkClearValue clear{};
    clear.color = { { 0.06f, 0.07f, 0.10f, 1.0f } };

    VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass.renderPass = g_renderPass;
    pass.framebuffer = g_framebuffers[imageIndex];
    pass.renderArea.extent = g_swapchainExtent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass_(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{ 0.0f, 0.0f,
                         static_cast<float>(g_swapchainExtent.width),
                         static_cast<float>(g_swapchainExtent.height), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, g_swapchainExtent };
    vkCmdSetViewport_(cmd, 0, 1, &viewport);
    vkCmdSetScissor_(cmd, 0, 1, &scissor);

    vkCmdBindPipeline_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
    const float push[2] = { angle,
                            static_cast<float>(g_swapchainExtent.width) /
                            static_cast<float>(g_swapchainExtent.height) };
    vkCmdPushConstants_(cmd, g_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkCmdDraw_(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass_(cmd);
    vkEndCommandBuffer_(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g_imageAvailable[slot];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g_renderFinished[slot];
    vkQueueSubmit_(g_queue, 1, &submit, g_inFlight[slot]);

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &g_renderFinished[slot];
    present.swapchainCount = 1;
    present.pSwapchains = &g_swapchain;
    present.pImageIndices = &imageIndex;

    VkResult presented = vkQueuePresentKHR_(g_queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) g_needsRecreate = true;

    ++g_frame;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double frameUs = static_cast<double>(now.QuadPart - g_lastFrameQpc.QuadPart) * 1e6
                   / static_cast<double>(g_qpcFreq.QuadPart);
    g_lastFrameQpc = now;

    if (frameUs > 0.0 && frameUs < 1e6) {
        g_frameTimes.push_back(frameUs / 1000.0);
        if (g_frameTimes.size() > 200000) {
            g_frameTimes.erase(g_frameTimes.begin(), g_frameTimes.begin() + 100000);
        }
        UpdateTitle(frameUs / 1000.0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_CHAR:
        ++g_inputReceived;
        break;
    default:
        break;
    }

    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;

    case WM_ENTERSIZEMOVE: g_inSizeMove = true;  return 0;
    case WM_EXITSIZEMOVE:  g_inSizeMove = false; g_needsRecreate = true; return 0;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && !g_inSizeMove) g_needsRecreate = true;
        return 0;

    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN) {
            SetDisplayMode(g_mode == DisplayMode::Exclusive ? DisplayMode::Windowed
                                                            : DisplayMode::Exclusive);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        ++g_inputReceived;
        switch (wparam) {
        case VK_ESCAPE: g_running = false; PostQuitMessage(0); return 0;
        case VK_F1: SetDisplayMode(DisplayMode::Windowed);   return 0;
        case VK_F2: SetDisplayMode(DisplayMode::Borderless); return 0;
        case VK_F3: SetDisplayMode(DisplayMode::Exclusive);  return 0;
        case 'V':   g_vsync = !g_vsync; g_frameTimes.clear(); g_needsRecreate = true; return 0;
        case 'C':   g_frameTimes.clear(); return 0;
        default: break;
        }
        break;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    // Declare per-monitor DPI awareness, as a real Vulkan game does through its
    // manifest. Without it Windows bitmap-stretches the window, so the swapchain
    // (physical) and the window client (logical) disagree by the DPI scale -
    // which throws off the overlay's pointer mapping. D3D games get this for
    // free because DXGI sets it implicitly; Vulkan touches no such API.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");
    g_vsync = cmd.find(L"-vsync") != std::wstring::npos;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_frameTimes.reserve(200000);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SampleGameVulkanWindow";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SampleGameVulkan", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) Fatal("CreateWindowEx");

    InitVulkan();
    ShowWindow(g_hwnd, SW_SHOW);
    GetWindowRect(g_hwnd, &g_windowedRect);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;
        RenderFrame();
    }

    if (g_device) vkDeviceWaitIdle_(g_device);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (g_imageAvailable[i]) vkDestroySemaphore_(g_device, g_imageAvailable[i], nullptr);
        if (g_renderFinished[i]) vkDestroySemaphore_(g_device, g_renderFinished[i], nullptr);
        if (g_inFlight[i]) vkDestroyFence_(g_device, g_inFlight[i], nullptr);
    }
    DestroySwapchainResources();
    if (g_swapchain) vkDestroySwapchainKHR_(g_device, g_swapchain, nullptr);
    if (g_commandPool) vkDestroyCommandPool_(g_device, g_commandPool, nullptr);
    if (g_pipeline) vkDestroyPipeline_(g_device, g_pipeline, nullptr);
    if (g_pipelineLayout) vkDestroyPipelineLayout_(g_device, g_pipelineLayout, nullptr);
    if (g_renderPass) vkDestroyRenderPass_(g_device, g_renderPass, nullptr);
    if (g_device) vkDestroyDevice_(g_device, nullptr);
    if (g_surface) vkDestroySurfaceKHR_(g_instance, g_surface, nullptr);
    if (g_instance) vkDestroyInstance_(g_instance, nullptr);
    return 0;
}
