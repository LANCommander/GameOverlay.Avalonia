using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// Entry point to the per-OS implementations of the overlay's platform seams.
/// Every operation the overlay needs that differs between Windows, Linux and
/// macOS is created here, so the rest of the library stays platform-neutral and
/// there is a single place the supported-platform matrix is expressed.
/// </summary>
/// <remarks>
/// The concrete backend is selected at compile time: each target framework
/// contributes one implementation of <see cref="CreateBackend"/> (the Windows
/// build supplies the Win32 backend; the portable build supplies a fallback
/// that grows a Linux/macOS backend in later phases). This keeps OS-specific
/// types out of the neutral compilation entirely rather than merely unused.
/// </remarks>
internal static partial class PlatformServices
{
    private static readonly IPlatformBackend Backend = CreateBackend();

    // Implemented once per target framework (Platform/Windows and the fallback).
    private static partial IPlatformBackend CreateBackend();

    public static IProcessInjector OpenProcess(int pid, Action<string>? log = null)
        => Backend.OpenProcess(pid, log);

    public static IProcessInjector LaunchSuspended(string exePath, string? arguments = null, Action<string>? log = null)
        => Backend.LaunchSuspended(exePath, arguments, log);

    public static IGlobalHotkey? CreateHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log = null)
        => Backend.CreateHotkey(hotkey, onToggle, log);

    public static IFrameProducer CreateFrameProducer(GameGraphicsApi api, IProcessInjector game, OverlaySharedState state, Action<string>? log = null)
        => Backend.CreateFrameProducer(api, game, state, log);

    public static IKeyMapper CreateKeyMapper()
        => Backend.CreateKeyMapper();

    public static ISharedMemory CreateOrOpenSharedMemory(string logicalName, long size)
        => Backend.CreateOrOpenSharedMemory(logicalName, size);
}
