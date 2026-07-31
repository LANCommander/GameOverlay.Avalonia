// OpenGL present hooks.
//
// Two entry points swap an OpenGL window: gdi32!SwapBuffers and
// opengl32!wglSwapBuffers. Games use either, so both are hooked; a thread-local
// re-entry guard stops a double-composite if one calls the other internally.
//
// Unlike Vulkan, OpenGL needs no extension enabled at context-creation time
// (WGL_NV_DX_interop works on any existing context), so the overlay can attach
// to an already-running GL game - no launch-suspended requirement.

#include "opengl_hooks.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <MinHook.h>

#include "hooks.h"
#include "input.h"
#include "log.h"
#include "opengl_renderer.h"
#include "shared_state.h"

namespace overlay {
namespace {

using SwapBuffersFn = BOOL(WINAPI*)(HDC);
using WglGetCurrentContextFn = HGLRC(WINAPI*)();

SwapBuffersFn g_originalSwapBuffers = nullptr;
SwapBuffersFn g_originalWglSwapBuffers = nullptr;
WglGetCurrentContextFn g_wglGetCurrentContext = nullptr;

OpenGLRenderer g_renderer;
bool           g_installed = false;
bool           g_published = false;
thread_local bool tls_inSwap = false;

void OnSwap(HDC hdc) {
    if (tls_inSwap) return;

    // A current GL context is what distinguishes a real GL present from an
    // ordinary GDI double-buffer swap on the same entry point.
    if (!g_wglGetCurrentContext || !g_wglGetCurrentContext()) return;

    SharedState* state = GetSharedState();
    if (!state) return;

    tls_inSwap = true;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->presentCount));

    if (!g_published) {
        g_published = true;
        state->graphicsApi = kGraphicsApiOpenGL;
        // LUID stays 0: the interop D3D device and the GL context must be on
        // the same GPU, so both use the default adapter. Correct on a
        // single-GPU machine; a multi-GPU setup would need the LUID from
        // GL_EXT_memory_object_win32.
        if (HWND hwnd = WindowFromDC(hdc)) {
            state->gameHwnd = reinterpret_cast<uint64_t>(hwnd);
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                state->gameWidth = rc.right - rc.left;
                state->gameHeight = rc.bottom - rc.top;
            }
            InstallInputHook(hwnd);
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));
        }
        OVERLAY_LOG("OpenGL present detected (hwnd 0x%llX, %ux%u)",
                    static_cast<unsigned long long>(state->gameHwnd), state->gameWidth, state->gameHeight);
    }

    // Keep the published size current across window resizes.
    if (HWND hwnd = WindowFromDC(hdc)) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            const uint32_t w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w && h && (w != state->gameWidth || h != state->gameHeight)) {
                state->gameWidth = w;
                state->gameHeight = h;
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->swapchainGeneration));
            }
        }
    }

    if (HostIsAlive(state)) {
        g_renderer.Render(hdc, state);
    } else if (state->inputCapture) {
        ForceReleaseCapture();
    }

    tls_inSwap = false;
}

BOOL WINAPI SwapBuffersDetour(HDC hdc) {
    OnSwap(hdc);
    return g_originalSwapBuffers(hdc);
}

BOOL WINAPI WglSwapBuffersDetour(HDC hdc) {
    OnSwap(hdc);
    return g_originalWglSwapBuffers(hdc);
}

bool HookExport(const wchar_t* module, const char* name, void* detour, void** original) {
    HMODULE handle = GetModuleHandleW(module);
    if (!handle) return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(handle, name));
    if (!target) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    return MH_EnableHook(target) == MH_OK;
}

} // namespace

bool InstallOpenGLHooks() {
    if (g_installed) return true;

    // gdi32 is always loaded, so this hook is always available and is what the
    // sample (and most GL apps) actually call.
    bool any = HookExport(L"gdi32.dll", "SwapBuffers",
                          reinterpret_cast<void*>(&SwapBuffersDetour),
                          reinterpret_cast<void**>(&g_originalSwapBuffers));

    // wglSwapBuffers only exists once opengl32 is loaded. Do not force-load it
    // into a D3D/Vulkan game that will never use it.
    if (GetModuleHandleW(L"opengl32.dll")) {
        any |= HookExport(L"opengl32.dll", "wglSwapBuffers",
                          reinterpret_cast<void*>(&WglSwapBuffersDetour),
                          reinterpret_cast<void**>(&g_originalWglSwapBuffers));
        g_wglGetCurrentContext = reinterpret_cast<WglGetCurrentContextFn>(
            GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetCurrentContext"));
    }

    if (!any) return false;

    g_installed = true;
    OVERLAY_LOG("OpenGL present hooks installed (SwapBuffers%s)",
                g_originalWglSwapBuffers ? ", wglSwapBuffers" : "");
    return true;
}

void RemoveOpenGLHooks() {
    if (!g_installed) return;
    g_renderer.Shutdown();
    g_installed = false;
}

} // namespace overlay
