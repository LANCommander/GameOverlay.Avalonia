#pragma once

namespace overlay {

// Installs the Direct3D 8 present hooks. Like the D3D9, OpenGL and Vulkan
// installers this is self-contained and best-effort: it returns false
// (harmlessly) on a machine with no d3d8.dll or when the probe device cannot be
// created, so a non-D3D8 game is never affected.
bool InstallD3D8Hooks();
void RemoveD3D8Hooks();

} // namespace overlay
