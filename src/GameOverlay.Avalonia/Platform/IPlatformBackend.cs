using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// The complete set of OS-specific services the overlay needs, created as one
/// unit per platform. <see cref="PlatformServices"/> resolves exactly one
/// backend for the running OS; the rest of the library depends only on the
/// neutral interfaces returned here.
/// </summary>
internal interface IPlatformBackend
{
    /// <summary>Opens an already-running game process for injection.</summary>
    IProcessInjector OpenProcess(int pid, Action<string>? log);

    /// <summary>Launches a game suspended so the payload can be injected before it runs.</summary>
    IProcessInjector LaunchSuspended(string exePath, string? arguments, Action<string>? log);

    /// <summary>
    /// Registers a global overlay-toggle hotkey, or returns null when the
    /// platform offers no global-hotkey mechanism.
    /// </summary>
    IGlobalHotkey? CreateHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log);

    /// <summary>Creates the frame transport appropriate to the game's graphics API.</summary>
    IFrameProducer CreateFrameProducer(GameGraphicsApi api, IProcessInjector game, OverlaySharedState state, Action<string>? log);

    /// <summary>Creates the key-code translator for input the payload reports.</summary>
    IKeyMapper CreateKeyMapper();

    /// <summary>
    /// Creates or opens a named cross-process shared-memory region of the given
    /// size, mapped read/write. <paramref name="logicalName"/> is prefix-free;
    /// the backend adds any OS-specific namespace prefix.
    /// </summary>
    ISharedMemory CreateOrOpenSharedMemory(string logicalName, long size);
}
