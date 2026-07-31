// linux_input.h - X11 input capture for the Linux payload.
//
// The Windows payload swallows input by subclassing the game's window procedure.
// X11 has no such per-message hook, so instead the payload opens its OWN X
// connection and actively grabs the keyboard and pointer (XGrabKeyboard /
// XGrabPointer) while the overlay is interactive. The X server then delivers
// those events to the payload's connection instead of the game's - the game is
// deaf to input exactly as it is on Windows - and a dedicated thread translates
// them into the shared input ring. Releasing the grab hands input back.
#pragma once

#include <cstdint>

namespace overlay {

// Starts grabbing keyboard + pointer to the given game window (an X11 Window
// XID) and pumping events into the ring. Idempotent.
void EnterLinuxCapture(uint64_t gameWindow);

// Releases the grab and stops forwarding. Idempotent.
void LeaveLinuxCapture();

// Tears the input connection down entirely (on detach).
void ShutdownLinuxInput();

}  // namespace overlay
