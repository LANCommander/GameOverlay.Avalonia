using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// A registered global hotkey that toggles the overlay from outside the game
/// process. Disposing it unregisters the hotkey.
/// </summary>
/// <remarks>
/// Implemented per platform: Windows uses a passive low-level keyboard hook
/// (<c>WH_KEYBOARD_LL</c>), Linux will use <c>XGrabKey</c>. The listener is
/// deliberately host-side - nothing is injected into the game to observe keys -
/// so a platform with no global-hotkey mechanism can simply provide none, and
/// the consumer drives <see cref="GameOverlay.Interactive"/> itself.
/// </remarks>
internal interface IGlobalHotkey : IDisposable
{
}
