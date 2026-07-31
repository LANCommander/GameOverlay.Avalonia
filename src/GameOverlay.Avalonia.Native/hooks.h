#pragma once

namespace overlay {

struct SharedState;

// Whether the host is still bumping its heartbeat. Shared by the DXGI and
// Vulkan present paths so both stop compositing on a dead host, and both
// release input capture rather than leaving the game deaf.
bool HostIsAlive(const SharedState* state);

// Installs trampoline hooks on the DXGI present path. Safe to call once, from
// a worker thread (never under the loader lock).
bool InstallHooks();

// Removes the hooks and tears down the renderer. After this returns, no
// overlay code is reachable from the game's render thread.
void RemoveHooks();

} // namespace overlay
