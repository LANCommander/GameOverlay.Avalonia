// vk_layer_linux.cpp - Linux Vulkan overlay layer.
//
// Vulkan on Linux is intercepted with a layer, not LD_PRELOAD: games resolve
// vkQueuePresentKHR through vkGetDeviceProcAddr, which bypasses the dynamic
// symbol an LD_PRELOAD interposer would override. This layer inserts itself into
// the instance/device dispatch chains, maps the shared control block (the same
// POSIX shm the GLX payload uses), and drives LinuxVulkanRenderer from
// vkQueuePresentKHR to composite the overlay into the swapchain image - the
// Vulkan analogue of linux_payload.cpp.

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

// vulkan_xlib.h pulls in X11 headers, which #define None/Bool - both collide
// with identifiers in the shared contract below, so drop the macros first.
#ifdef None
#undef None
#endif
#ifdef Bool
#undef Bool
#endif

#include "host_liveness.h"
#include "linux_input.h"
#include "linux_vulkan_renderer.h"
#include "log.h"
#include "platform/platform.h"
#include "shared_state.h"

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {
using namespace overlay;

inline void* DispatchKey(void* handle) { return *reinterpret_cast<void**>(handle); }

struct InstanceData {
    PFN_vkGetInstanceProcAddr NextGIPA = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR = nullptr;
};

struct DeviceData {
    PFN_vkGetDeviceProcAddr NextGDPA = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
};

std::mutex g_lock;
std::map<void*, InstanceData> g_instances;
std::map<void*, DeviceData> g_devices;

VkInstance g_instance = VK_NULL_HANDLE;
uint32_t g_queueFamily = 0;
LinuxVulkanRenderer g_renderer;

platform::SharedMapping g_mapping;
SharedBlock* g_block = nullptr;
bool g_initTried = false;
uint64_t g_gameWindow = 0;   // X11 Window from the xlib surface
bool g_capturing = false;    // input-capture state
HostLiveness g_hostLive;

void EnsureInit() {
    if (g_initTried) return;
    g_initTried = true;
    const uint32_t pid = platform::CurrentProcessId();
    g_mapping = platform::MapSharedBlock(pid, sizeof(SharedBlock));
    if (!g_mapping.base) { OVERLAY_LOG("vk layer: shared block map failed"); return; }
    g_block = static_cast<SharedBlock*>(g_mapping.base);
    g_block->state.gamePid = pid;
    g_block->state.graphicsApi = kGraphicsApiVulkan;
    g_block->state.abiVersion = kAbiVersion;
    g_block->state.dllAttached = 1;
    OVERLAY_LOG("vk layer attached to pid %u (Vulkan)", pid);
}

VkLayerInstanceCreateInfo* InstanceChainInfo(const VkInstanceCreateInfo* ci, VkLayerFunction func) {
    auto* p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && p->function == func))
        p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(p->pNext));
    return p;
}

VkLayerDeviceCreateInfo* DeviceChainInfo(const VkDeviceCreateInfo* ci, VkLayerFunction func) {
    auto* p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == func))
        p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext));
    return p;
}

// --- hooked entry points ---------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL OverlayQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present) {
    EnsureInit();
    DeviceData dd;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        dd = g_devices[DispatchKey(queue)];
    }
    if (!g_block || present->swapchainCount == 0) return dd.QueuePresentKHR(queue, present);

    SharedState* state = &g_block->state;
    state->presentCount++;

    // A dead host must not leave the overlay frozen or the game deaf.
    if (!g_hostLive.Alive(state)) {
        if (g_capturing) { LeaveLinuxCapture(); g_capturing = false; }
        OVERLAY_LOG_ONCE("host is not responding; overlay disabled");
        return dd.QueuePresentKHR(queue, present);
    }

    // Follow the host's capture toggle, grabbing input away from the game
    // through the X window the surface was created on (same path as GLX).
    if (g_gameWindow != 0) {
        state->gameHwnd = g_gameWindow;
        bool wantCapture = state->inputCapture != 0;
        if (wantCapture && !g_capturing) { EnterLinuxCapture(g_gameWindow); g_capturing = true; }
        else if (!wantCapture && g_capturing) { LeaveLinuxCapture(); g_capturing = false; }
    }

    VkSemaphore overlaySem = VK_NULL_HANDLE;
    if (state->visible) {
        overlaySem = g_renderer.Render(queue, present->pSwapchains[0], present->pImageIndices[0],
                                       present->pWaitSemaphores, present->waitSemaphoreCount, state);
    }

    if (overlaySem != VK_NULL_HANDLE) {
        // Our submit already waited on the game's semaphores, so present must now
        // wait on ours instead (mirrors the Windows QueuePresentHook).
        VkPresentInfoKHR patched = *present;
        patched.waitSemaphoreCount = 1;
        patched.pWaitSemaphores = &overlaySem;
        return dd.QueuePresentKHR(queue, &patched);
    }
    return dd.QueuePresentKHR(queue, present);
}

VKAPI_ATTR void VKAPI_CALL OverlayGetDeviceQueue(VkDevice device, uint32_t queueFamily,
                                                 uint32_t queueIndex, VkQueue* pQueue) {
    PFN_vkGetDeviceQueue down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_devices[DispatchKey(device)].GetDeviceQueue;
    }
    down(device, queueFamily, queueIndex, pQueue);
    if (queueFamily == g_queueFamily) g_renderer.OnQueue(*pQueue, queueFamily);
}

VKAPI_ATTR VkResult VKAPI_CALL OverlayCreateSwapchainKHR(VkDevice device,
                                                        const VkSwapchainCreateInfoKHR* ci,
                                                        const VkAllocationCallbacks* alloc,
                                                        VkSwapchainKHR* pSwapchain) {
    EnsureInit();
    PFN_vkCreateSwapchainKHR down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_devices[DispatchKey(device)].CreateSwapchainKHR;
    }
    VkResult r = down(device, ci, alloc, pSwapchain);
    if (r == VK_SUCCESS && g_block) {
        g_block->state.gameWidth = ci->imageExtent.width;
        g_block->state.gameHeight = ci->imageExtent.height;
        g_block->state.swapchainGeneration++;
        g_renderer.OnSwapchainCreated(*pSwapchain, ci->imageFormat, ci->imageExtent);
        OVERLAY_LOG("vk layer: swapchain %ux%u fmt=%d", ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL OverlayDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      const VkAllocationCallbacks* alloc) {
    g_renderer.OnSwapchainDestroyed(swapchain);
    PFN_vkDestroySwapchainKHR down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_devices[DispatchKey(device)].DestroySwapchainKHR;
    }
    down(device, swapchain, alloc);
}

VKAPI_ATTR void VKAPI_CALL OverlayDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    g_renderer.Shutdown();
    PFN_vkDestroyDevice down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        void* key = DispatchKey(device);
        down = g_devices[key].DestroyDevice;
        g_devices.erase(key);
    }
    down(device, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL OverlayCreateXlibSurfaceKHR(VkInstance instance,
                                                          const VkXlibSurfaceCreateInfoKHR* ci,
                                                          const VkAllocationCallbacks* alloc,
                                                          VkSurfaceKHR* pSurface) {
    PFN_vkCreateXlibSurfaceKHR down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_instances[DispatchKey(instance)].CreateXlibSurfaceKHR;
    }
    VkResult r = down(instance, ci, alloc, pSurface);
    if (r == VK_SUCCESS) {
        g_gameWindow = static_cast<uint64_t>(ci->window);
        OVERLAY_LOG("vk layer: xlib surface window 0x%lx", static_cast<unsigned long>(ci->window));
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL OverlayDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        void* key = DispatchKey(instance);
        down = g_instances[key].DestroyInstance;
        g_instances.erase(key);
    }
    down(instance, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL OverlayGetDeviceProcAddr(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL OverlayGetInstanceProcAddr(VkInstance instance, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL OverlayCreateDevice(VkPhysicalDevice physicalDevice,
                                                   const VkDeviceCreateInfo* ci,
                                                   const VkAllocationCallbacks* alloc,
                                                   VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* link = DeviceChainInfo(ci, VK_LAYER_LINK_INFO);
    PFN_vkGetInstanceProcAddr nextGIPA = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGDPA = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(nextGIPA(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!createDevice) return VK_ERROR_INITIALIZATION_FAILED;

    if (ci->queueCreateInfoCount > 0) g_queueFamily = ci->pQueueCreateInfos[0].queueFamilyIndex;

    // Enable VK_EXT_external_memory_host (+ its dependency) if the device
    // supports it, so the renderer can import the CPU frame buffer directly as
    // image memory (zero-copy) rather than uploading it every present. Mirrors
    // the Windows path adding external-memory extensions at device create.
    bool importEnabled = false;
    std::vector<const char*> extensions(ci->ppEnabledExtensionNames,
                                        ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    if (auto enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            nextGIPA(g_instance, "vkEnumerateDeviceExtensionProperties"))) {
        uint32_t ec = 0;
        enumExt(physicalDevice, nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> avail(ec);
        enumExt(physicalDevice, nullptr, &ec, avail.data());
        bool hasHost = false, hasExtMem = false;
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) hasHost = true;
            if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0) hasExtMem = true;
        }
        if (hasHost && hasExtMem) {
            auto ensure = [&](const char* name) {
                for (const char* x : extensions) if (std::strcmp(x, name) == 0) return;
                extensions.push_back(name);
            };
            ensure(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
            ensure(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
            importEnabled = true;
        }
    }
    VkDeviceCreateInfo ci2 = *ci;
    ci2.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci2.ppEnabledExtensionNames = extensions.data();

    VkResult r = createDevice(physicalDevice, &ci2, alloc, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceData d;
    d.NextGDPA = nextGDPA;
    d.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(nextGDPA(*pDevice, "vkDestroyDevice"));
    d.QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(nextGDPA(*pDevice, "vkQueuePresentKHR"));
    d.GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(nextGDPA(*pDevice, "vkGetDeviceQueue"));
    d.CreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(nextGDPA(*pDevice, "vkCreateSwapchainKHR"));
    d.DestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(nextGDPA(*pDevice, "vkDestroySwapchainKHR"));
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_devices[DispatchKey(*pDevice)] = d;
    }
    g_renderer.Init(g_instance, physicalDevice, *pDevice, nextGIPA, nextGDPA, importEnabled);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL OverlayCreateInstance(const VkInstanceCreateInfo* ci,
                                                     const VkAllocationCallbacks* alloc,
                                                     VkInstance* pInstance) {
    VkLayerInstanceCreateInfo* link = InstanceChainInfo(ci, VK_LAYER_LINK_INFO);
    PFN_vkGetInstanceProcAddr nextGIPA = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(nextGIPA(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = createInstance(ci, alloc, pInstance);
    if (r != VK_SUCCESS) return r;

    InstanceData d;
    d.NextGIPA = nextGIPA;
    d.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(nextGIPA(*pInstance, "vkDestroyInstance"));
    d.CreateXlibSurfaceKHR = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(nextGIPA(*pInstance, "vkCreateXlibSurfaceKHR"));
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_instances[DispatchKey(*pInstance)] = d;
    }
    g_instance = *pInstance;
    return r;
}

#define RETURN_IF(n, fn) if (std::strcmp(name, n) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)

PFN_vkVoidFunction VKAPI_CALL OverlayGetDeviceProcAddr(VkDevice device, const char* name) {
    RETURN_IF("vkGetDeviceProcAddr", OverlayGetDeviceProcAddr);
    RETURN_IF("vkDestroyDevice", OverlayDestroyDevice);
    RETURN_IF("vkQueuePresentKHR", OverlayQueuePresentKHR);
    RETURN_IF("vkGetDeviceQueue", OverlayGetDeviceQueue);
    RETURN_IF("vkCreateSwapchainKHR", OverlayCreateSwapchainKHR);
    RETURN_IF("vkDestroySwapchainKHR", OverlayDestroySwapchainKHR);

    PFN_vkGetDeviceProcAddr down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_devices[DispatchKey(device)].NextGDPA;
    }
    return down ? down(device, name) : nullptr;
}

PFN_vkVoidFunction VKAPI_CALL OverlayGetInstanceProcAddr(VkInstance instance, const char* name) {
    RETURN_IF("vkGetInstanceProcAddr", OverlayGetInstanceProcAddr);
    RETURN_IF("vkCreateInstance", OverlayCreateInstance);
    RETURN_IF("vkDestroyInstance", OverlayDestroyInstance);
    RETURN_IF("vkCreateDevice", OverlayCreateDevice);
    RETURN_IF("vkGetDeviceProcAddr", OverlayGetDeviceProcAddr);
    RETURN_IF("vkDestroyDevice", OverlayDestroyDevice);
    RETURN_IF("vkQueuePresentKHR", OverlayQueuePresentKHR);
    RETURN_IF("vkGetDeviceQueue", OverlayGetDeviceQueue);
    RETURN_IF("vkCreateSwapchainKHR", OverlayCreateSwapchainKHR);
    RETURN_IF("vkDestroySwapchainKHR", OverlayDestroySwapchainKHR);
    RETURN_IF("vkCreateXlibSurfaceKHR", OverlayCreateXlibSurfaceKHR);

    if (instance == VK_NULL_HANDLE) return nullptr;
    PFN_vkGetInstanceProcAddr down;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        down = g_instances[DispatchKey(instance)].NextGIPA;
    }
    return down ? down(instance, name) : nullptr;
}

#undef RETURN_IF

}  // namespace

namespace overlay {
// linux_input.cpp reaches the shared block through these (mirroring dllmain.cpp
// on Windows and linux_payload.cpp for GLX).
SharedState* GetSharedState() { return g_block ? &g_block->state : nullptr; }
InputRing*   GetInputRing()   { return g_block ? &g_block->input : nullptr; }
}  // namespace overlay

// --- loader interface ------------------------------------------------------

extern "C" VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct->loaderLayerInterfaceVersion > 2)
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = OverlayGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = OverlayGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
