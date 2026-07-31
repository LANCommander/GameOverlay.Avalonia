namespace GameOverlay.Avalonia;

// Windows build's contribution to the platform-service selector.
internal static partial class PlatformServices
{
    private static partial IPlatformBackend CreateBackend() => new WindowsPlatformBackend();
}
