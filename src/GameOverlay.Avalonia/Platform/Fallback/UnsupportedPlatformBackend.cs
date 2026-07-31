using System;
using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// The backend for target frameworks that have no OS-specific implementation
/// yet (the portable <c>net10.0</c> build). Injection and GPU sharing are not
/// available, so those operations fail clearly; a Linux/macOS backend replaces
/// this in later phases.
/// </summary>
internal sealed class UnsupportedPlatformBackend : IPlatformBackend
{
    private const string Message =
        "The overlay has no native backend for this platform yet. Linux and macOS " +
        "support is in progress; today the overlay runs on Windows.";

    public IProcessInjector OpenProcess(int pid, Action<string>? log)
        => throw new PlatformNotSupportedException(Message);

    public IProcessInjector LaunchSuspended(string exePath, string? arguments, Action<string>? log)
        => throw new PlatformNotSupportedException(Message);

    public IGlobalHotkey? CreateHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log)
    {
        log?.Invoke("[hotkey] no global-hotkey support on this platform; drive Interactive manually.");
        return null;
    }

    public IFrameProducer CreateFrameProducer(GameGraphicsApi api, IProcessInjector game, OverlaySharedState state, Action<string>? log)
        => throw new PlatformNotSupportedException(Message);

    public ISharedMemory CreateOrOpenSharedMemory(string logicalName, long size)
        => throw new PlatformNotSupportedException(Message);

    public IKeyMapper CreateKeyMapper() => new NullKeyMapper();

    private sealed class NullKeyMapper : IKeyMapper
    {
        public Key KeyFromVirtualKey(int virtualKey, int keyData) => Key.None;
    }
}
