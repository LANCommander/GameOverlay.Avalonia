using System;
using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>Linux implementation of the overlay's platform services (Phase 1: GLX/X11, CPU transport).</summary>
internal sealed class LinuxPlatformBackend : IPlatformBackend
{
    public IProcessInjector OpenProcess(int pid, Action<string>? log)
        => throw new PlatformNotSupportedException(
            "Attaching to a running game is not supported on Linux yet; launch it via the overlay instead.");

    public IProcessInjector LaunchSuspended(string exePath, string? arguments, Action<string>? log)
        => LinuxProcessInjector.Launch(exePath, arguments, log);

    public IGlobalHotkey? CreateHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log)
    {
        try
        {
            return new LinuxGlobalHotkey(hotkey, onToggle, log);
        }
        catch (Exception e)
        {
            // No X display (e.g. headless without X) - the consumer can still
            // drive Interactive itself.
            log?.Invoke($"[hotkey] global hotkey unavailable: {e.Message}");
            return null;
        }
    }

    public IFrameProducer CreateFrameProducer(GameGraphicsApi api, IProcessInjector game, OverlaySharedState state, Action<string>? log)
        // Phase 1 Linux uses the CPU shared-memory transport for every API (the
        // GLX payload uploads these pixels). GPU sharing (dma-buf) is Phase 2.
        => new CpuFrameProducer(state, log);

    public IKeyMapper CreateKeyMapper() => new LinuxKeyMapper();

    public ISharedMemory CreateOrOpenSharedMemory(string logicalName, long size)
        => new LinuxSharedMemory(logicalName, size);
}
