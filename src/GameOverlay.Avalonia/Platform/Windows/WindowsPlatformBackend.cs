using System;
using Avalonia.Input;
using Avalonia.Win32.Input;

namespace GameOverlay.Avalonia;

/// <summary>Windows implementation of the overlay's platform services.</summary>
internal sealed class WindowsPlatformBackend : IPlatformBackend
{
    public IProcessInjector OpenProcess(int pid, Action<string>? log)
        => GameProcess.Open(pid, log);

    public IProcessInjector LaunchSuspended(string exePath, string? arguments, Action<string>? log)
        => GameProcess.LaunchSuspended(exePath, arguments, log);

    public IGlobalHotkey? CreateHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log)
        => new HotkeyListener(hotkey, onToggle, log);

    public IFrameProducer CreateFrameProducer(GameGraphicsApi api, IProcessInjector game, OverlaySharedState state, Action<string>? log)
        // D3D8 and D3D9 have no GPU-shared texture path, so they take the CPU
        // shared-memory transport; everything else shares a GPU texture.
        => api is GameGraphicsApi.D3D8 or GameGraphicsApi.D3D9
            ? new CpuFrameProducer(state, log)
            : new SharedTextureProducer(game, state, log);

    public IKeyMapper CreateKeyMapper() => new WindowsKeyMapper();

    public ISharedMemory CreateOrOpenSharedMemory(string logicalName, long size)
        => new WindowsSharedMemory(logicalName, size);
}

/// <summary>Maps Win32 virtual keys to Avalonia keys via the Win32 platform helper.</summary>
internal sealed class WindowsKeyMapper : IKeyMapper
{
    // Avalonia.Win32 ships no reference assembly, so this call resolves against
    // the runtime assembly; it needs the lParam key data to tell left/right
    // modifiers apart.
    public Key KeyFromVirtualKey(int virtualKey, int keyData)
        => KeyInterop.KeyFromVirtualKey(virtualKey, keyData);
}
