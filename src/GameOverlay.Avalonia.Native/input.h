#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace overlay {

// Subclasses the game's window so input can be intercepted. Idempotent, and a
// no-op until a valid HWND is known.
void InstallInputHook(HWND hwnd);

// Restores the original window procedure. Safe to call if nothing was
// installed.
void RemoveInputHook();

// Called when the host stops responding, so a crashed host cannot leave the
// game permanently unable to receive its own input.
void ForceReleaseCapture();

} // namespace overlay
