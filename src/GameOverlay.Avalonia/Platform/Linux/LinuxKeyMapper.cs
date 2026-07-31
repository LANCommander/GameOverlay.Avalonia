using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// Maps X11 keysyms (which the Linux payload reports in <c>InputEvent.data</c>)
/// to Avalonia keys.
/// </summary>
/// <remarks>
/// Avalonia's <see cref="Key"/> enum follows WPF's numeric values, where the
/// letter and digit ranges are contiguous, so those map by offset; the rest are
/// listed explicitly. Keysym constants are from <c>X11/keysymdef.h</c>.
/// </remarks>
internal sealed class LinuxKeyMapper : IKeyMapper
{
    // Latin block.
    private const int XK_space = 0x0020;
    private const int XK_0 = 0x0030, XK_9 = 0x0039;
    private const int XK_A = 0x0041, XK_Z = 0x005a;
    private const int XK_a = 0x0061, XK_z = 0x007a;

    // Function-key and control blocks (0xFFxx).
    private const int XK_BackSpace = 0xFF08, XK_Tab = 0xFF09, XK_Return = 0xFF0D, XK_Escape = 0xFF1B;
    private const int XK_Home = 0xFF50, XK_Left = 0xFF51, XK_Up = 0xFF52, XK_Right = 0xFF53, XK_Down = 0xFF54;
    private const int XK_Prior = 0xFF55, XK_Next = 0xFF56, XK_End = 0xFF57, XK_Insert = 0xFF63;
    private const int XK_F1 = 0xFFBE, XK_F12 = 0xFFC9;
    private const int XK_Shift_L = 0xFFE1, XK_Shift_R = 0xFFE2, XK_Control_L = 0xFFE3, XK_Control_R = 0xFFE4;
    private const int XK_Alt_L = 0xFFE9, XK_Alt_R = 0xFFEA, XK_Super_L = 0xFFEB, XK_Super_R = 0xFFEC;
    private const int XK_Delete = 0xFFFF;

    public Key KeyFromVirtualKey(int virtualKey, int keyData)
    {
        int ks = virtualKey;

        if (ks >= XK_a && ks <= XK_z) return Key.A + (ks - XK_a);
        if (ks >= XK_A && ks <= XK_Z) return Key.A + (ks - XK_A);
        if (ks >= XK_0 && ks <= XK_9) return Key.D0 + (ks - XK_0);
        if (ks >= XK_F1 && ks <= XK_F12) return Key.F1 + (ks - XK_F1);

        return ks switch
        {
            XK_space => Key.Space,
            XK_Return => Key.Enter,
            XK_Escape => Key.Escape,
            XK_BackSpace => Key.Back,
            XK_Tab => Key.Tab,
            XK_Left => Key.Left,
            XK_Up => Key.Up,
            XK_Right => Key.Right,
            XK_Down => Key.Down,
            XK_Home => Key.Home,
            XK_End => Key.End,
            XK_Prior => Key.PageUp,
            XK_Next => Key.PageDown,
            XK_Insert => Key.Insert,
            XK_Delete => Key.Delete,
            XK_Shift_L => Key.LeftShift,
            XK_Shift_R => Key.RightShift,
            XK_Control_L => Key.LeftCtrl,
            XK_Control_R => Key.RightCtrl,
            XK_Alt_L => Key.LeftAlt,
            XK_Alt_R => Key.RightAlt,
            XK_Super_L => Key.LWin,
            XK_Super_R => Key.RWin,
            _ => Key.None,
        };
    }
}
