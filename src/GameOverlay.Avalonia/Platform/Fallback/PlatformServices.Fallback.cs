using System;

namespace GameOverlay.Avalonia;

// Portable (non-Windows) build's contribution to the platform-service selector.
// Picks the Linux backend when running on Linux; other non-Windows platforms
// (macOS) fall back to the unsupported backend until their phase lands.
internal static partial class PlatformServices
{
    private static partial IPlatformBackend CreateBackend()
        => OperatingSystem.IsLinux() ? new LinuxPlatformBackend() : new UnsupportedPlatformBackend();
}
