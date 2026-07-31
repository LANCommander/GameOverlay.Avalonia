#pragma once

namespace overlay {

// Hooks the OpenGL present path (SwapBuffers / wglSwapBuffers). Returns false
// if neither could be hooked. Cheap for a non-GL game: gdi32 is always loaded,
// and the hook passes straight through when there is no current GL context.
bool InstallOpenGLHooks();

void RemoveOpenGLHooks();

} // namespace overlay
