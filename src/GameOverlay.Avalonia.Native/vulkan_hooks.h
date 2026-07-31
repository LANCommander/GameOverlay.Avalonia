#pragma once

namespace overlay {

// Hooks the Vulkan entry points exported by vulkan-1.dll. Returns false when
// the process has no Vulkan loader, which is the normal case for a D3D game.
bool InstallVulkanHooks();

void RemoveVulkanHooks();

} // namespace overlay
