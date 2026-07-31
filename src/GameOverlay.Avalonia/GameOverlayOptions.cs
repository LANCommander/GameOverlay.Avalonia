using System;
using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// Configuration for a <see cref="GameOverlay"/>.
/// </summary>
public sealed class GameOverlayOptions
{
    /// <summary>
    /// Fixes the overlay's UI scale. When null (the default) the scale is derived
    /// from the game's resolution so the UI stays a similar physical size from
    /// 720p to 4K.
    /// </summary>
    public double? Scaling { get; set; }

    /// <summary>Overlay redraw/upload cadence, independent of the game's frame rate.</summary>
    public int TargetFps { get; set; } = 60;

    /// <summary>
    /// A global hotkey that toggles <see cref="GameOverlay.Interactive"/>. Set to
    /// null to install no built-in hotkey (drive <see cref="GameOverlay.Interactive"/>
    /// yourself).
    /// </summary>
    public OverlayHotkey? ToggleHotkey { get; set; } = new(Key.F1, KeyModifiers.Shift);

    /// <summary>Whether the overlay starts visible and capturing input.</summary>
    public bool StartInteractive { get; set; }

    /// <summary>
    /// Explicit path to the native payload (<c>GameOverlay.Native.dll</c>). When
    /// null it is discovered next to the application, under
    /// <c>runtimes/win-x64/native</c>, or in a dev <c>build/bin</c> tree.
    /// </summary>
    public string? PayloadPath { get; set; }

    /// <summary>How long to wait for the injected payload's handshake.</summary>
    public TimeSpan AttachTimeout { get; set; } = TimeSpan.FromSeconds(20);

    /// <summary>
    /// Optional sink for operational diagnostics (injection, handshake, publish
    /// counters). The library writes nothing anywhere by default.
    /// </summary>
    public Action<string>? DiagnosticLog { get; set; }
}
