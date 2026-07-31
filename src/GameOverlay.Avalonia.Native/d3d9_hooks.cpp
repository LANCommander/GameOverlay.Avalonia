// Direct3D 9 present hooks.
//
// D3D9 has no DXGI, so it is hooked exactly like OpenGL and Vulkan are: by
// reading the device vtable from a throwaway probe device and patching the
// functions in place with MinHook. We hook EndScene (where the game is mid-scene
// and the backbuffer is the current target, so the overlay can be drawn) and
// Reset (to drop our D3DPOOL_DEFAULT texture before the device is reset).
//
// Direct3DCreate9 is resolved with GetProcAddress rather than an import, so this
// translation unit never forces d3d9.dll to load into a game that does not use
// it - the same rule the OpenGL hooks follow for opengl32.

#include "d3d9_hooks.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include "d3d9_renderer.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "shared_state.h"

namespace overlay {
namespace {

// IDirect3DDevice9 vtable slots. Fixed by COM binary compatibility, so hardcoding
// them is safe - the same values every D3D9 overlay has relied on for 20 years.
//   16 Reset
//   17 Present
//   42 EndScene
constexpr int kSlotReset = 16;
constexpr int kSlotEndScene = 42;

using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

EndSceneFn g_originalEndScene = nullptr;
ResetFn    g_originalReset = nullptr;

D3D9Renderer g_renderer9;
bool         g_installed = false;
bool         g_published = false;
thread_local bool tls_inEndScene = false;

// Reads the swapchain's window and backbuffer size. hDeviceWindow can be null on
// a device created with only a focus window, so fall back to that.
void ResolveWindowAndSize(IDirect3DDevice9* device, HWND& hwnd, uint32_t& width, uint32_t& height) {
    hwnd = nullptr;
    width = height = 0;

    IDirect3DSwapChain9* swapChain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(swapChain->GetPresentParameters(&pp))) {
            hwnd = pp.hDeviceWindow;
            width = pp.BackBufferWidth;
            height = pp.BackBufferHeight;
        }
        swapChain->Release();
    }

    if (!hwnd) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (SUCCEEDED(device->GetCreationParameters(&cp))) hwnd = cp.hFocusWindow;
    }
}

void PublishSize(IDirect3DDevice9* device, SharedState* state, bool first) {
    HWND hwnd = nullptr;
    uint32_t width = 0, height = 0;
    ResolveWindowAndSize(device, hwnd, width, height);
    if (width == 0 || height == 0) return;

    if (first) {
        state->graphicsApi = kGraphicsApiD3D9;
        // LUID stays 0: the CPU transport shares no GPU resource, so there is no
        // adapter to match against - the payload uploads on the game's own device.
        state->gameHwnd = reinterpret_cast<uint64_t>(hwnd);
        state->gameWidth = width;
        state->gameHeight = height;
        if (hwnd) InstallInputHook(hwnd);
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));
        OVERLAY_LOG("D3D9 present detected (hwnd 0x%llX, %ux%u)",
                    static_cast<unsigned long long>(state->gameHwnd), width, height);
        return;
    }

    if (width != state->gameWidth || height != state->gameHeight) {
        state->gameWidth = width;
        state->gameHeight = height;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));
    }
}

HRESULT STDMETHODCALLTYPE EndSceneDetour(IDirect3DDevice9* device) {
    if (!tls_inEndScene) {
        tls_inEndScene = true;

        if (SharedState* state = GetSharedState()) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->presentCount));

            if (!g_published) {
                g_published = true;
                PublishSize(device, state, /*first=*/true);
            } else {
                PublishSize(device, state, /*first=*/false);
            }

            if (HostIsAlive(state)) {
                g_renderer9.Render(device, state);
            } else if (state->inputCapture) {
                ForceReleaseCapture();
            }
        }

        tls_inEndScene = false;
    }
    return g_originalEndScene(device);
}

HRESULT STDMETHODCALLTYPE ResetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    // The DEFAULT-pool texture must be gone before Reset, or Reset fails outright.
    g_renderer9.OnDeviceLost();

    HRESULT hr = g_originalReset(device, params);

    if (SUCCEEDED(hr)) {
        if (SharedState* state = GetSharedState()) {
            PublishSize(device, state, /*first=*/false);
        }
    }
    return hr;
}

// Creates a throwaway device and captures the EndScene and Reset function
// pointers from its vtable, then releases it. The pointers themselves stay valid
// because the functions live in d3d9.dll, which remains loaded - but the vtable
// *array* can be a per-object heap allocation that Release frees, so we must
// read the slots we need before releasing and never keep the vtable pointer.
bool CaptureTargets(void*& endScene, void*& reset) {
    void** vtable = nullptr;
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) return false;   // not a D3D9 game; nothing to hook

    auto create = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!create) return false;

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) {
        OVERLAY_LOG("Direct3DCreate9 returned null");
        return false;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AvaloniaOverlayD3D9Probe";
    RegisterClassExW(&wc);

    // A real, non-degenerate window: a 1x1 window has essentially no client
    // area, and deriving a backbuffer from that is what made CreateDevice fail
    // with D3DERR_DRIVERINTERNALERROR. The window is never shown.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        d3d->Release();
        OVERLAY_LOG("D3D9 probe window creation failed: %lu", GetLastError());
        return false;
    }

    // Backbuffer derived from the window (UNKNOWN format + zero size), the
    // canonical dummy-device recipe. D3DCREATE_MULTITHREADED because the game is
    // rendering on its own thread while we create this second device in the same
    // process; without it the runtime is not thread-safe and can fault.
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    const DWORD behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING
                         | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behavior, &pp, &device);
    if (FAILED(hr) || !device) {
        // Fall back to the reference rasterizer purely to read the vtable; the
        // slots are identical regardless of device type.
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd, behavior, &pp, &device);
    }

    if (FAILED(hr) || !device) {
        DestroyWindow(hwnd);
        d3d->Release();
        OVERLAY_LOG("D3D9 probe device creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    // Read the two slots we need *now*, while the device (and therefore its
    // vtable) is still alive; keeping the vtable pointer to dereference after
    // Release would be a use-after-free on drivers with per-object vtables.
    vtable = *reinterpret_cast<void***>(device);
    endScene = vtable[kSlotEndScene];
    reset = vtable[kSlotReset];

    device->Release();
    DestroyWindow(hwnd);
    d3d->Release();
    return true;
}

} // namespace

bool InstallD3D9Hooks() {
    if (g_installed) return true;

    void* endScene = nullptr;
    void* reset = nullptr;
    if (!CaptureTargets(endScene, reset)) return false;

    bool ok = MH_CreateHook(endScene, reinterpret_cast<void*>(&EndSceneDetour),
                            reinterpret_cast<void**>(&g_originalEndScene)) == MH_OK;
    ok &= MH_CreateHook(reset, reinterpret_cast<void*>(&ResetDetour),
                        reinterpret_cast<void**>(&g_originalReset)) == MH_OK;
    if (ok) {
        ok &= MH_EnableHook(endScene) == MH_OK;
        ok &= MH_EnableHook(reset) == MH_OK;
    }

    if (!ok) {
        OVERLAY_LOG("failed to hook D3D9 EndScene/Reset");
        return false;
    }

    g_installed = true;
    OVERLAY_LOG("D3D9 present hooks installed (EndScene, Reset)");
    return true;
}

void RemoveD3D9Hooks() {
    if (!g_installed) return;
    g_renderer9.Shutdown();
    g_installed = false;
}

} // namespace overlay
