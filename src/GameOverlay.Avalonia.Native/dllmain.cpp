// Entry point for the injected payload.
//
// DllMain does almost nothing on purpose: it runs under the loader lock, where
// creating D3D devices, loading libraries or waiting on anything is a deadlock
// waiting to happen. All real initialization happens on a worker thread.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hooks.h"
#include "log.h"
#include "platform/platform.h"
#include "shared_state.h"

namespace overlay {
namespace {

platform::SharedMapping g_mapping;
SharedBlock*            g_block = nullptr;
HMODULE                 g_module = nullptr;

bool InitSharedState() {
    const uint32_t pid = platform::CurrentProcessId();

    g_mapping = platform::MapSharedBlock(pid, sizeof(SharedBlock));
    if (!g_mapping.base) return false;
    g_block = static_cast<SharedBlock*>(g_mapping.base);

    // The DLL owns abiVersion. The host treats a matching value plus
    // dllAttached as its signal that the payload is live and the contract is
    // one it understands.
    g_block->state.gamePid = pid;
    g_block->state.abiVersion = kAbiVersion;

    OVERLAY_LOG("shared block mapped at %p (%zu bytes)",
                static_cast<void*>(g_block), sizeof(SharedBlock));
    return true;
}

void ReleaseSharedState() {
    if (g_block) {
        g_block->state.dllAttached = 0;
        g_block = nullptr;
    }
    platform::UnmapSharedBlock(g_mapping, sizeof(SharedBlock));
}

DWORD WINAPI InitThread(LPVOID) {
    OVERLAY_LOG("payload attaching to pid %lu", GetCurrentProcessId());

    if (!InitSharedState()) return 1;

    if (!InstallHooks()) {
        OVERLAY_LOG("hook installation failed; payload is inert");
        ReleaseSharedState();
        return 1;
    }
    return 0;
}

} // namespace

SharedState* GetSharedState() { return g_block ? &g_block->state : nullptr; }
InputRing*   GetInputRing()   { return g_block ? &g_block->input : nullptr; }

} // namespace overlay

// Clean detach. The host calls this via CreateRemoteThread so the overlay can
// be removed without restarting the game.
extern "C" __declspec(dllexport) DWORD WINAPI OverlayDetach(LPVOID) {
    overlay::RemoveHooks();
    overlay::ReleaseSharedState();

    HMODULE self = overlay::g_module;
    OVERLAY_LOG("payload detached");
    if (self) {
        FreeLibraryAndExitThread(self, 0);   // does not return
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        overlay::g_module = module;
        DisableThreadLibraryCalls(module);

        // Everything of substance is deferred off the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, overlay::InitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }

    case DLL_PROCESS_DETACH:
        // reserved != nullptr means the process is terminating. The loader is
        // tearing everything down around us and touching D3D or waiting on a
        // thread here would hang or crash the exit path; let the OS reclaim it.
        if (reserved == nullptr) {
            overlay::RemoveHooks();
            overlay::ReleaseSharedState();
        }
        break;

    default:
        break;
    }
    return TRUE;
}
