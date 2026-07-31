#pragma once

namespace overlay {

// Installs the Direct3D 9 present hooks. Like the OpenGL and Vulkan installers
// this is self-contained and best-effort: it returns false (harmlessly) on a
// machine with no d3d9.dll or when the probe device cannot be created, so a
// non-D3D9 game is never affected.
bool InstallD3D9Hooks();
void RemoveD3D9Hooks();

} // namespace overlay
