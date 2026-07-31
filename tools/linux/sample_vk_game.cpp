// sample_vk_game.cpp - a minimal Vulkan/Xlib "game", the Linux counterpart of
// SampleGameVulkan. It clears the screen and presents in a loop through an Xlib
// surface (so the overlay layer's vkCreateXlibSurfaceKHR hook captures the
// window for input), letting the layer composite the overlay over it.
//
// Build: g++ sample_vk_game.cpp -o sample_vk_game -lvulkan -lX11

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { std::fprintf(stderr, #x " = %d\n", _r); return 2; } } while (0)

namespace {
constexpr uint32_t kW = 800, kH = 600;
}

int main(int argc, char** argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 600;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "XOpenDisplay failed\n"); return 2; }
    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, kW, kH, 0, 0, 0);
    XStoreName(dpy, win, "SampleVkGame");
    XSelectInput(dpy, win, StructureNotifyMask);
    XMapWindow(dpy, win);

    const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    VkInstance instance;
    VKCHECK(vkCreateInstance(&ici, nullptr, &instance));

    VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = dpy;
    sci.window = win;
    VkSurfaceKHR surface;
    VKCHECK(vkCreateXlibSurfaceKHR(instance, &sci, nullptr, &surface));

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());
    VkPhysicalDevice phys = pds[0];

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, qprops.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = i; break; }
    }
    if (family == UINT32_MAX) { std::fprintf(stderr, "no graphics+present queue\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    VkDevice device;
    VKCHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    VkExtent2D extent = caps.currentExtent.width != 0xFFFFFFFF ? caps.currentExtent : VkExtent2D{kW, kH};
    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = imgCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    VKCHECK(vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain));

    uint32_t scImgCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &scImgCount, nullptr);
    std::vector<VkImage> images(scImgCount);
    vkGetSwapchainImagesKHR(device, swapchain, &scImgCount, images.data());

    // Render pass: clear the frame and leave it PRESENT_SRC.
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VkRenderPass renderPass;
    VKCHECK(vkCreateRenderPass(device, &rpi, nullptr, &renderPass));

    std::vector<VkImageView> views(scImgCount);
    std::vector<VkFramebuffer> fbs(scImgCount);
    std::vector<VkSemaphore> renderDone(scImgCount);
    for (uint32_t i = 0; i < scImgCount; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(device, &vi, nullptr, &views[i]));
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &views[i];
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(device, &fi, nullptr, &fbs[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &si, nullptr, &renderDone[i]);
    }

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(device, &cpi, nullptr, &pool));
    std::vector<VkCommandBuffer> cmds(scImgCount);
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = scImgCount;
    vkAllocateCommandBuffers(device, &cbi, cmds.data());
    for (uint32_t i = 0; i < scImgCount; i++) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmds[i], &bi);
        VkClearValue clear{};
        clear.color = {{0.10f, 0.10f, 0.15f, 1.0f}};   // dark scene
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = renderPass;
        rb.framebuffer = fbs[i];
        rb.renderArea = {{0, 0}, extent};
        rb.clearValueCount = 1;
        rb.pClearValues = &clear;
        vkCmdBeginRenderPass(cmds[i], &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmds[i]);
        vkEndCommandBuffer(cmds[i]);
    }

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imageAvailable;
    vkCreateSemaphore(device, &si, nullptr, &imageAvailable);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence inFlight;
    vkCreateFence(device, &fci, nullptr, &inFlight);

    std::printf("sample vk game pid %d, %d frames, %ux%u\n", (int)getpid(), frames, extent.width, extent.height);
    std::fflush(stdout);

    for (int f = 0; f < frames; f++) {
        vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &inFlight);
        uint32_t idx = 0;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &idx) != VK_SUCCESS)
            break;

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmds[idx];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderDone[idx];
        vkQueueSubmit(queue, 1, &submit, inFlight);

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderDone[idx];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &idx;
        vkQueuePresentKHR(queue, &present);
        usleep(8000);
    }

    vkDeviceWaitIdle(device);
    return 0;
}
