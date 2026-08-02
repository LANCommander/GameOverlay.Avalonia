#include "hooks.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>

#include "d3d8_hooks.h"
#include "d3d9_hooks.h"
#include "d3d10_renderer.h"
#include "d3d11_renderer.h"
#include "d3d12_renderer.h"
#include "input.h"
#include "log.h"
#include "opengl_hooks.h"
#include "shared_state.h"
#include "vulkan_hooks.h"

#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace overlay {
namespace {

// Vtable slots on the DXGI swapchain interfaces. These are fixed by COM
// binary compatibility and cannot change without breaking every shipped
// D3D application, so hardcoding them is safe.
//
//   IUnknown              0..2   QueryInterface / AddRef / Release
//   IDXGIObject           3..6   Set/GetPrivateData, SetPrivateDataInterface, GetParent
//   IDXGIDeviceSubObject  7      GetDevice
//   IDXGISwapChain        8..17  Present, GetBuffer, Set/GetFullscreenState, GetDesc,
//                                ResizeBuffers, ResizeTarget, GetContainingOutput, ...
//   IDXGISwapChain1       18..28 GetDesc1, GetFullscreenDesc, GetHwnd, GetCoreWindow,
//                                Present1, ...
constexpr int kSlotPresent = 8;
constexpr int kSlotResizeBuffers = 13;
constexpr int kSlotPresent1 = 22;

// ID3D12CommandQueue vtable:
//   IUnknown            0..2
//   ID3D12Object        3..6   Get/SetPrivateData, SetPrivateDataInterface, SetName
//   ID3D12DeviceChild   7      GetDevice
//   ID3D12Pageable      -      (adds nothing)
//   ID3D12CommandQueue  8..    UpdateTileMappings, CopyTileMappings,
//                              ExecuteCommandLists, ...
constexpr int kSlotExecuteCommandLists = 10;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn       g_originalPresent = nullptr;
Present1Fn      g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                      ID3D12CommandList* const*);
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

D3D10Renderer g_renderer10;
D3D11Renderer g_renderer;
D3D12Renderer g_renderer12;
bool          g_hooksInstalled = false;

// Which API the swapchain belongs to. Resolved once, on the first Present.
enum class GraphicsApi { Unknown, D3D10, D3D11, D3D12, Unsupported };
GraphicsApi g_api = GraphicsApi::Unknown;

// The game's direct command queue, caught in flight. D3D12 exposes no way to
// get it from the swapchain, so hooking ExecuteCommandLists is the only route.
ID3D12CommandQueue* volatile g_gameQueue = nullptr;
LONG                        g_queueHookRequested = 0;

// Guards against re-entering our own compositing path. A game that presents
// from more than one thread, or a driver that internally re-enters Present,
// would otherwise corrupt the renderer's state.
thread_local bool tls_inPresent = false;

void STDMETHODCALLTYPE ExecuteCommandListsDetour(ID3D12CommandQueue* queue, UINT count,
                                                ID3D12CommandList* const* lists) {
    // Hot path: once captured this is a single load and a predictable branch.
    // Games submit on copy and compute queues too, so only a DIRECT queue is
    // usable for drawing into the backbuffer.
    if (!g_gameQueue && queue) {
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(&g_gameQueue), queue, nullptr);
        }
    }
    g_originalExecuteCommandLists(queue, count, lists);
}

// Installs the queue hook on a worker thread. MinHook suspends threads when
// enabling a hook, so doing this from inside the game's Present would risk
// deadlocking against ourselves.
DWORD WINAPI InstallQueueHookThread(LPVOID parameter) {
    auto* device = static_cast<ID3D12Device*>(parameter);

    // Read the vtable from a throwaway queue on the game's own device rather
    // than spinning up a second D3D12 device just to look at it.
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue* probe = nullptr;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe));
    if (FAILED(hr) || !probe) {
        OVERLAY_LOG("probe command queue creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        device->Release();
        return 1;
    }

    void** vtable = *reinterpret_cast<void***>(probe);
    bool ok = MH_CreateHook(vtable[kSlotExecuteCommandLists],
                            reinterpret_cast<void*>(&ExecuteCommandListsDetour),
                            reinterpret_cast<void**>(&g_originalExecuteCommandLists)) == MH_OK;
    if (ok) ok = MH_EnableHook(vtable[kSlotExecuteCommandLists]) == MH_OK;

    probe->Release();
    device->Release();

    OVERLAY_LOG(ok ? "ID3D12CommandQueue::ExecuteCommandLists hooked"
                   : "failed to hook ExecuteCommandLists");
    return ok ? 0 : 1;
}

void RequestQueueHook(ID3D12Device* device) {
    if (InterlockedExchange(&g_queueHookRequested, 1) != 0) return;

    device->AddRef();   // released by the worker
    HANDLE thread = CreateThread(nullptr, 0, InstallQueueHookThread, device, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        device->Release();
        OVERLAY_LOG("could not start queue hook thread: %lu", GetLastError());
    }
}

// Resolves the swapchain's API and publishes the adapter it lives on.
//
// This runs before either renderer initializes, because the host needs the
// adapter LUID to create its device on the right GPU - and for D3D12 the
// renderer cannot start at all until a command queue has been captured, which
// takes at least one more frame.
void PublishDeviceInfo(IDXGISwapChain* swapChain, SharedState* state) {
    // D3D10 is checked *before* D3D11, and the order is load-bearing. On modern
    // Windows the D3D10 runtime is layered on top of D3D11 ("10-on-11"), so a
    // genuine D3D10 device also answers a QI for ID3D11Device - a D3D11-first
    // check would misclassify every D3D10 game as D3D11 and then fail to open
    // the texture with the hybrid device. A native D3D11 device, by contrast,
    // never exposes ID3D10Device, so this ordering catches D3D10 precisely while
    // leaving real D3D11 games to fall through.
    //
    // D3D10 also predates NT-handle sharing, which is why it needs the
    // host-side legacy-shared texture rather than the D3D11 path's NT handle.
    ID3D10Device* device10 = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device10))) && device10) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device10->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    LARGE_INTEGER luid{};
                    luid.LowPart = desc.AdapterLuid.LowPart;
                    luid.HighPart = desc.AdapterLuid.HighPart;
                    state->adapterLuid = static_cast<uint64_t>(luid.QuadPart);
                    OVERLAY_LOG("D3D10 game on %ls (LUID 0x%016llX)", desc.Description,
                                static_cast<unsigned long long>(state->adapterLuid));
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        device10->Release();
        g_api = GraphicsApi::D3D10;
        state->graphicsApi = kGraphicsApiD3D10;
        return;
    }

    ID3D11Device* device11 = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device11))) && device11) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    LARGE_INTEGER luid{};
                    luid.LowPart = desc.AdapterLuid.LowPart;
                    luid.HighPart = desc.AdapterLuid.HighPart;
                    state->adapterLuid = static_cast<uint64_t>(luid.QuadPart);
                    OVERLAY_LOG("D3D11 game on %ls (LUID 0x%016llX)", desc.Description,
                                static_cast<unsigned long long>(state->adapterLuid));
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        device11->Release();
        g_api = GraphicsApi::D3D11;
        state->graphicsApi = kGraphicsApiD3D11;
        return;
    }

    ID3D12Device* device12 = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device12))) && device12) {
        const LUID luid = device12->GetAdapterLuid();
        LARGE_INTEGER wide{};
        wide.LowPart = luid.LowPart;
        wide.HighPart = luid.HighPart;
        state->adapterLuid = static_cast<uint64_t>(wide.QuadPart);
        OVERLAY_LOG("D3D12 game (LUID 0x%016llX)",
                    static_cast<unsigned long long>(state->adapterLuid));

        g_api = GraphicsApi::D3D12;
        state->graphicsApi = kGraphicsApiD3D12;
        RequestQueueHook(device12);
        device12->Release();
        return;
    }

    g_api = GraphicsApi::Unsupported;
    state->graphicsApi = kGraphicsApiUnknown;
    OVERLAY_LOG_ONCE("swapchain belongs to neither ID3D11Device nor ID3D12Device; unsupported");
}

void PublishSwapChainInfo(IDXGISwapChain* swapChain, SharedState* state) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;

    state->gameHwnd = reinterpret_cast<uint64_t>(desc.OutputWindow);
    state->gameWidth = desc.BufferDesc.Width;
    state->gameHeight = desc.BufferDesc.Height;

    // The swapchain's output window is the one that receives input, so this is
    // the earliest point at which we know what to subclass.
    InstallInputHook(desc.OutputWindow);
    // Publishing the size here rather than lazily during rendering is what
    // breaks the startup cycle: the host needs our dimensions before it can
    // create the texture, and we need its texture before we can render.
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));

    OVERLAY_LOG("swapchain published: %ux%u hwnd=0x%llX gen=%u",
                desc.BufferDesc.Width, desc.BufferDesc.Height,
                static_cast<unsigned long long>(state->gameHwnd), state->swapchainGeneration);
}

// The host bumps hostHeartbeat continuously. If it stops, the host has died or
// been killed, and the payload must stop compositing rather than leave a frozen
// frame pasted over the game forever.
//
// GetTickCount64 reads shared user data rather than making a syscall, so this
// is affordable on every Present even at several thousand frames per second.
} // namespace

bool HostIsAlive(const SharedState* state) {
    static uint32_t lastSeen = 0;
    static ULONGLONG lastChangeTick = 0;

    const uint32_t heartbeat = state->hostHeartbeat;
    const ULONGLONG now = GetTickCount64();

    if (heartbeat != lastSeen) {
        lastSeen = heartbeat;
        lastChangeTick = now;
        return true;
    }

    if (lastChangeTick == 0) {
        lastChangeTick = now;
        return true;
    }

    constexpr ULONGLONG kHostTimeoutMs = 2000;
    return (now - lastChangeTick) < kHostTimeoutMs;
}

namespace {

void OnPresent(IDXGISwapChain* swapChain, UINT flags) {
    // DXGI_PRESENT_TEST is a "can I present?" probe that must not produce any
    // output; drawing on it would be both wrong and wasteful.
    if (flags & DXGI_PRESENT_TEST) return;
    if (tls_inPresent) return;

    SharedState* state = GetSharedState();
    if (!state) return;

    tls_inPresent = true;

    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->presentCount));

    // Resolve the API and publish the adapter before anything else. On D3D12
    // the renderer cannot start until a command queue has been captured, and
    // the host must not be left waiting on an adapter LUID until then.
    static bool published = false;
    if (g_api == GraphicsApi::Unknown) {
        PublishDeviceInfo(swapChain, state);
    }
    if (!published && g_api != GraphicsApi::Unknown && g_api != GraphicsApi::Unsupported) {
        published = true;
        PublishSwapChainInfo(swapChain, state);
    }

    const bool hostAlive = HostIsAlive(state);

    switch (g_api) {
    case GraphicsApi::D3D10:
        if (g_renderer10.EnsureInitialized(swapChain, state) && hostAlive) {
            g_renderer10.Render(swapChain, state);
        }
        break;

    case GraphicsApi::D3D11:
        if (g_renderer.EnsureInitialized(swapChain, state) && hostAlive) {
            g_renderer.Render(swapChain, state);
        }
        break;

    case GraphicsApi::D3D12:
        if (g_renderer12.EnsureInitialized(swapChain, g_gameQueue, state) && hostAlive) {
            g_renderer12.Render(swapChain, state);
        }
        break;

    default:
        break;
    }

    if (!hostAlive && state->inputCapture) {
        // A host that dies while holding capture would otherwise leave the game
        // permanently unable to receive its own input.
        ForceReleaseCapture();
    }

    tls_inPresent = false;
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    OnPresent(swapChain, flags);
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* swapChain, UINT syncInterval,
                                         UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    OnPresent(swapChain, flags);
    return g_originalPresent1(swapChain, syncInterval, flags, params);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* swapChain, UINT bufferCount,
                                              UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    // Drop our cached views *and* the references we hold on the old buffers
    // first: an outstanding reference makes ResizeBuffers fail outright.
    g_renderer10.OnResizeBuffers();
    g_renderer.OnResizeBuffers();
    g_renderer12.OnResizeBuffers();

    HRESULT hr = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);

    if (SUCCEEDED(hr)) {
        if (SharedState* state = GetSharedState()) {
            PublishSwapChainInfo(swapChain, state);
        }
    }
    return hr;
}

// Creates a throwaway swapchain purely to read the DXGI vtable. The interfaces
// are released immediately; the function pointers stay valid because they live
// in dxgi.dll, which remains loaded for the life of the process.
bool CaptureVtables(void**& swapChainVtable, void**& swapChain1Vtable) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AvaloniaOverlayProbe";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        OVERLAY_LOG("probe window creation failed: %lu", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    const D3D_DRIVER_TYPE driverTypes[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : driverTypes) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &scd, &swapChain,
                                           &device, nullptr, &context);
        if (SUCCEEDED(hr)) break;
    }

    if (FAILED(hr) || !swapChain) {
        OVERLAY_LOG("probe swapchain creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        return false;
    }

    swapChainVtable = *reinterpret_cast<void***>(swapChain);

    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
        swapChain1Vtable = *reinterpret_cast<void***>(swapChain1);
        swapChain1->Release();
    } else {
        swapChain1Vtable = nullptr;
        OVERLAY_LOG("IDXGISwapChain1 unavailable; Present1 will not be hooked");
    }

    if (context) context->Release();
    if (device) device->Release();
    swapChain->Release();
    DestroyWindow(hwnd);
    return true;
}

} // namespace

bool InstallHooks() {
    if (g_hooksInstalled) return true;

    if (MH_Initialize() != MH_OK) {
        OVERLAY_LOG("MH_Initialize failed");
        return false;
    }

    // Vulkan, OpenGL, D3D9 and D3D8 first, and independently: a game using one
    // of those may have no usable D3D11 device to probe the DXGI vtable with,
    // and failing that probe must not take those paths down with it. The
    // fixed-function D3D8/9 paths in particular share nothing with DXGI.
    const bool vulkanHooked = InstallVulkanHooks();
    const bool openglHooked = InstallOpenGLHooks();
    const bool d3d9Hooked = InstallD3D9Hooks();
    const bool d3d8Hooked = InstallD3D8Hooks();

    void** vtable = nullptr;
    void** vtable1 = nullptr;
    if (!CaptureVtables(vtable, vtable1)) {
        if (vulkanHooked || openglHooked || d3d9Hooked || d3d8Hooked) {
            g_hooksInstalled = true;
            if (SharedState* state = GetSharedState()) state->dllAttached = 1;
            OVERLAY_LOG("DXGI probe failed but non-DXGI hooks are live (Vulkan:%d OpenGL:%d D3D9:%d D3D8:%d)",
                        vulkanHooked, openglHooked, d3d9Hooked, d3d8Hooked);
            return true;
        }
        MH_Uninitialize();
        return false;
    }

    // MinHook patches the target function itself rather than swapping the
    // vtable pointer, so we coexist with other overlays already hooked into
    // the same process instead of clobbering them.
    bool ok = true;
    ok &= MH_CreateHook(vtable[kSlotPresent], reinterpret_cast<void*>(&PresentDetour),
                        reinterpret_cast<void**>(&g_originalPresent)) == MH_OK;
    ok &= MH_CreateHook(vtable[kSlotResizeBuffers], reinterpret_cast<void*>(&ResizeBuffersDetour),
                        reinterpret_cast<void**>(&g_originalResizeBuffers)) == MH_OK;
    if (vtable1) {
        ok &= MH_CreateHook(vtable1[kSlotPresent1], reinterpret_cast<void*>(&Present1Detour),
                            reinterpret_cast<void**>(&g_originalPresent1)) == MH_OK;
    }

    if (!ok) {
        OVERLAY_LOG("MH_CreateHook failed");
        MH_Uninitialize();
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OVERLAY_LOG("MH_EnableHook failed");
        MH_Uninitialize();
        return false;
    }

    g_hooksInstalled = true;
    if (SharedState* state = GetSharedState()) {
        state->dllAttached = 1;
    }
    OVERLAY_LOG("hooks installed (Present%s, ResizeBuffers%s%s%s%s)",
                vtable1 ? ", Present1" : "", vulkanHooked ? ", Vulkan" : "",
                openglHooked ? ", OpenGL" : "", d3d9Hooked ? ", D3D9" : "",
                d3d8Hooked ? ", D3D8" : "");
    return true;
}

void RemoveHooks() {
    if (!g_hooksInstalled) return;

    RemoveInputHook();

    MH_DisableHook(MH_ALL_HOOKS);

    // The game's render thread may be inside a detour right now. MinHook's
    // disable does not wait for that, so give in-flight calls a moment to
    // unwind before we tear down the resources they are using.
    Sleep(100);

    g_renderer10.Shutdown();
    g_renderer.Shutdown();
    RemoveVulkanHooks();
    RemoveOpenGLHooks();
    RemoveD3D9Hooks();
    RemoveD3D8Hooks();
    MH_Uninitialize();
    g_hooksInstalled = false;

    if (SharedState* state = GetSharedState()) {
        state->dllAttached = 0;
    }
    OVERLAY_LOG("hooks removed");
}

} // namespace overlay
