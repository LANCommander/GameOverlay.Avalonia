// Vulkan entry point hooks.
//
// Unlike D3D, none of this can be done after the fact. Vulkan will not let a
// device import external memory unless VK_KHR_external_memory_win32 was
// enabled when that device was created, and devices cannot be re-created. So
// the payload has to be in the process before the game calls vkCreateDevice,
// and rewrites the extension list on the way through.
//
// That is why the host launches Vulkan games suspended and injects first.

#include "vulkan_hooks.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <MinHook.h>

#include <cstring>
#include <vector>

#include "hooks.h"
#include "input.h"
#include "log.h"
#include "shared_state.h"
#include "vulkan_renderer.h"

namespace overlay {
namespace {

HMODULE g_loader = nullptr;
bool    g_installed = false;

PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
VkInstance                g_instance = VK_NULL_HANDLE;
VkPhysicalDevice          g_physicalDevice = VK_NULL_HANDLE;

VulkanRenderer g_renderer;

PFN_vkCreateInstance      g_originalCreateInstance = nullptr;
PFN_vkCreateDevice        g_originalCreateDevice = nullptr;
PFN_vkGetDeviceQueue      g_originalGetDeviceQueue = nullptr;
PFN_vkCreateSwapchainKHR  g_originalCreateSwapchain = nullptr;
PFN_vkDestroySwapchainKHR g_originalDestroySwapchain = nullptr;
PFN_vkQueuePresentKHR     g_originalQueuePresent = nullptr;
PFN_vkCreateWin32SurfaceKHR g_originalCreateWin32Surface = nullptr;

thread_local bool tls_inPresent = false;
bool g_inputHookTried = false;

struct FindWindowContext {
    DWORD pid;
    HWND  result;
};

BOOL CALLBACK FindProcessWindowProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<FindWindowContext*>(param);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != context->pid) return TRUE;               // keep scanning
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;

    context->result = hwnd;
    return FALSE;                                             // found it, stop
}

// Vulkan reveals the game's HWND only through vkCreateWin32SurfaceKHR, which is
// called exactly once and very early - so early that the payload's hooks, being
// installed on a worker thread, can lose the race to it. Falling back to
// enumerating the process's own top-level window makes input capture robust
// regardless of that timing.
void EnsureInputHookFromWindow() {
    if (g_inputHookTried) return;
    g_inputHookTried = true;

    FindWindowContext context{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindProcessWindowProc, reinterpret_cast<LPARAM>(&context));
    if (context.result) {
        if (SharedState* state = GetSharedState()) {
            state->gameHwnd = reinterpret_cast<uint64_t>(context.result);
        }
        InstallInputHook(context.result);
    }
}

bool Contains(const char* const* list, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(list[i], name) == 0) return true;
    }
    return false;
}

VkResult VKAPI_CALL CreateInstanceHook(const VkInstanceCreateInfo* info,
                                       const VkAllocationCallbacks* allocator,
                                       VkInstance* instance) {
    // The capabilities extensions are needed to query external handle support.
    // On a 1.1+ instance they are already core, but a game targeting 1.0 has to
    // be topped up.
    std::vector<const char*> extensions(
        info->ppEnabledExtensionNames,
        info->ppEnabledExtensionNames + info->enabledExtensionCount);

    for (const char* wanted : { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME }) {
        if (!Contains(extensions.data(), static_cast<uint32_t>(extensions.size()), wanted)) {
            extensions.push_back(wanted);
        }
    }

    VkInstanceCreateInfo patched = *info;
    patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    patched.ppEnabledExtensionNames = extensions.data();

    VkResult result = g_originalCreateInstance(&patched, allocator, instance);
    if (result != VK_SUCCESS) {
        // A driver may reject our additions; never take the game down with us.
        OVERLAY_LOG("vkCreateInstance rejected the added extensions (0x%X); retrying unmodified",
                    static_cast<unsigned>(result));
        result = g_originalCreateInstance(info, allocator, instance);
    }
    if (result == VK_SUCCESS) g_instance = *instance;
    return result;
}

VkResult VKAPI_CALL CreateDeviceHook(VkPhysicalDevice physicalDevice,
                                     const VkDeviceCreateInfo* info,
                                     const VkAllocationCallbacks* allocator,
                                     VkDevice* device) {
    std::vector<const char*> extensions(
        info->ppEnabledExtensionNames,
        info->ppEnabledExtensionNames + info->enabledExtensionCount);

    // Everything needed to import the host's D3D11 texture and acquire its
    // keyed mutex. VK_KHR_win32_keyed_mutex is what lets Vulkan reuse the
    // D3D11 synchronisation protocol verbatim instead of needing its own.
    for (const char* wanted : { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
                                VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME }) {
        if (!Contains(extensions.data(), static_cast<uint32_t>(extensions.size()), wanted)) {
            extensions.push_back(wanted);
        }
    }

    VkDeviceCreateInfo patched = *info;
    patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    patched.ppEnabledExtensionNames = extensions.data();

    VkResult result = g_originalCreateDevice(physicalDevice, &patched, allocator, device);
    if (result != VK_SUCCESS) {
        OVERLAY_LOG("vkCreateDevice rejected the added extensions (0x%X); retrying unmodified - "
                    "the overlay will not be able to import the host texture",
                    static_cast<unsigned>(result));
        return g_originalCreateDevice(physicalDevice, info, allocator, device);
    }

    g_physicalDevice = physicalDevice;
    g_renderer.OnDeviceCreated(physicalDevice, *device, g_loader, GetSharedState());
    return result;
}

void VKAPI_CALL GetDeviceQueueHook(VkDevice device, uint32_t queueFamily,
                                   uint32_t queueIndex, VkQueue* queue) {
    g_originalGetDeviceQueue(device, queueFamily, queueIndex, queue);
    if (queue && *queue) g_renderer.OnQueue(*queue, queueFamily);
}

// The surface is the only place Vulkan reveals which window the game renders
// to, and the input hook needs that HWND.
VkResult VKAPI_CALL CreateWin32SurfaceHook(VkInstance instance,
                                           const VkWin32SurfaceCreateInfoKHR* info,
                                           const VkAllocationCallbacks* allocator,
                                           VkSurfaceKHR* surface) {
    VkResult result = g_originalCreateWin32Surface(instance, info, allocator, surface);
    if (result == VK_SUCCESS && info->hwnd) {
        if (SharedState* state = GetSharedState()) {
            state->gameHwnd = reinterpret_cast<uint64_t>(info->hwnd);
        }
        InstallInputHook(info->hwnd);
        OVERLAY_LOG("Vulkan surface on hwnd 0x%p", static_cast<void*>(info->hwnd));
    }
    return result;
}

VkResult VKAPI_CALL CreateSwapchainHook(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                        const VkAllocationCallbacks* allocator,
                                        VkSwapchainKHR* swapchain) {
    VkResult result = g_originalCreateSwapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS && swapchain) {
        g_renderer.OnSwapchainCreated(*swapchain, info->imageFormat, info->imageExtent);

        if (SharedState* state = GetSharedState()) {
            state->gameWidth = info->imageExtent.width;
            state->gameHeight = info->imageExtent.height;
            state->graphicsApi = kGraphicsApiVulkan;
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));
        }
    }
    return result;
}

void VKAPI_CALL DestroySwapchainHook(VkDevice device, VkSwapchainKHR swapchain,
                                     const VkAllocationCallbacks* allocator) {
    g_renderer.OnSwapchainDestroyed(swapchain);
    g_originalDestroySwapchain(device, swapchain, allocator);
}

VkResult VKAPI_CALL QueuePresentHook(VkQueue queue, const VkPresentInfoKHR* info) {
    SharedState* state = GetSharedState();
    if (!state || tls_inPresent || info->swapchainCount == 0) {
        return g_originalQueuePresent(queue, info);
    }

    tls_inPresent = true;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->presentCount));

    // The surface hook may have been missed (see EnsureInputHookFromWindow);
    // present is guaranteed to run, so guarantee the input hook from here.
    EnsureInputHookFromWindow();

    VkSemaphore overlaySemaphore = VK_NULL_HANDLE;
    if (HostIsAlive(state)) {
        overlaySemaphore = g_renderer.Render(queue, info->pSwapchains[0], info->pImageIndices[0],
                                             info->pWaitSemaphores, info->waitSemaphoreCount, state);
    } else if (state->inputCapture) {
        ForceReleaseCapture();
    }

    tls_inPresent = false;

    // Our submit already waited on the game's semaphores, so present must now
    // wait on ours instead - otherwise it would either race the overlay draw or
    // wait on semaphores that have already been consumed.
    if (overlaySemaphore != VK_NULL_HANDLE) {
        VkPresentInfoKHR patched = *info;
        patched.waitSemaphoreCount = 1;
        patched.pWaitSemaphores = &overlaySemaphore;
        return g_originalQueuePresent(queue, &patched);
    }

    return g_originalQueuePresent(queue, info);
}

bool Hook(const char* name, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(g_loader, name));
    if (!target) {
        OVERLAY_LOG("vulkan-1.dll does not export %s", name);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    return MH_EnableHook(target) == MH_OK;
}

} // namespace

bool InstallVulkanHooks() {
    if (g_installed) return true;

    // The payload is injected into a suspended process, so a game that loads
    // the Vulkan loader dynamically has not done so yet - there would be
    // nothing to hook, and polling for it would race the game's own
    // vkCreateInstance.
    //
    // Loading it ourselves closes that race: the loader is reference counted,
    // so the game's later LoadLibrary returns the very module we hooked. On a
    // machine with no Vulkan runtime this simply fails and the Vulkan path
    // stays disabled.
    g_loader = GetModuleHandleW(L"vulkan-1.dll");
    if (!g_loader) g_loader = LoadLibraryW(L"vulkan-1.dll");
    if (!g_loader) return false;

    g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_loader, "vkGetInstanceProcAddr"));
    if (!g_getInstanceProcAddr) {
        OVERLAY_LOG("vulkan-1.dll has no vkGetInstanceProcAddr");
        return false;
    }

    bool ok = true;
    ok &= Hook("vkCreateInstance", reinterpret_cast<void*>(&CreateInstanceHook),
               reinterpret_cast<void**>(&g_originalCreateInstance));
    ok &= Hook("vkCreateDevice", reinterpret_cast<void*>(&CreateDeviceHook),
               reinterpret_cast<void**>(&g_originalCreateDevice));
    ok &= Hook("vkGetDeviceQueue", reinterpret_cast<void*>(&GetDeviceQueueHook),
               reinterpret_cast<void**>(&g_originalGetDeviceQueue));
    ok &= Hook("vkCreateSwapchainKHR", reinterpret_cast<void*>(&CreateSwapchainHook),
               reinterpret_cast<void**>(&g_originalCreateSwapchain));
    ok &= Hook("vkDestroySwapchainKHR", reinterpret_cast<void*>(&DestroySwapchainHook),
               reinterpret_cast<void**>(&g_originalDestroySwapchain));
    ok &= Hook("vkQueuePresentKHR", reinterpret_cast<void*>(&QueuePresentHook),
               reinterpret_cast<void**>(&g_originalQueuePresent));
    ok &= Hook("vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(&CreateWin32SurfaceHook),
               reinterpret_cast<void**>(&g_originalCreateWin32Surface));

    if (!ok) {
        OVERLAY_LOG("failed to install the Vulkan hooks");
        return false;
    }

    g_installed = true;
    OVERLAY_LOG("Vulkan hooks installed");
    return true;
}

void RemoveVulkanHooks() {
    if (!g_installed) return;
    g_renderer.Shutdown();
    g_installed = false;
}

} // namespace overlay
