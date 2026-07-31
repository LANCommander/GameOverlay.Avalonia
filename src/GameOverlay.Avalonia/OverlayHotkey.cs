using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// A global key combination that toggles the overlay's interactive state.
/// </summary>
/// <remarks>
/// Expressed in Avalonia terms (<see cref="Avalonia.Input.Key"/> +
/// <see cref="KeyModifiers"/>); the library maps it to a Win32 virtual key
/// internally. The listener is a passive low-level keyboard hook - it observes
/// the combination without swallowing it, so the game still sees the keys.
/// </remarks>
public readonly record struct OverlayHotkey(Key Key, KeyModifiers Modifiers = KeyModifiers.None);
