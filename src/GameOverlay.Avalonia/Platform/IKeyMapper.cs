using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// Translates the platform key codes the payload reports into Avalonia keys.
/// </summary>
/// <remarks>
/// The payload's input ring carries raw OS key codes (Win32 virtual keys today);
/// only the platform that produced them knows how to map them to Avalonia's
/// <see cref="Key"/> enum. Windows wraps <c>Avalonia.Win32.Input.KeyInterop</c>;
/// other platforms provide their own mapping.
/// </remarks>
internal interface IKeyMapper
{
    /// <summary>
    /// Maps a platform key code (with any auxiliary key data the platform needs
    /// to disambiguate, e.g. the Win32 lParam scan-code bits) to an Avalonia key.
    /// </summary>
    Key KeyFromVirtualKey(int virtualKey, int keyData);
}
