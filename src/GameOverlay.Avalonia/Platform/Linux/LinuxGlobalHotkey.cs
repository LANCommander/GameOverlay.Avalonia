using System;
using System.Runtime.InteropServices;
using System.Threading;
using Avalonia.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// Linux <see cref="IGlobalHotkey"/>: a global overlay-toggle hotkey via an
/// X11 passive key grab on the root window.
/// </summary>
/// <remarks>
/// Host-side, like the Windows low-level keyboard hook - nothing is injected into
/// the game. A dedicated X connection grabs the key combo on the root window and
/// a thread fires the toggle on each matching press. Because the grab only
/// delivers that one combo, any KeyPress the connection receives is the hotkey,
/// so the event body never has to be parsed.
/// </remarks>
internal sealed partial class LinuxGlobalHotkey : IGlobalHotkey
{
    private const uint ShiftMask = 1, LockMask = 2, ControlMask = 4, Mod1Mask = 8, Mod2Mask = 16, Mod4Mask = 64;
    private const int GrabModeAsync = 1;
    private const int KeyPress = 2;

    private readonly IntPtr _display;
    private readonly nuint _root;
    private readonly int _keycode;
    private readonly uint _modifiers;
    private readonly Action _onToggle;
    private readonly Action<string>? _log;
    private readonly Thread _thread;
    private volatile bool _running = true;

    public LinuxGlobalHotkey(OverlayHotkey hotkey, Action onToggle, Action<string>? log)
    {
        _onToggle = onToggle;
        _log = log;

        _display = XOpenDisplay(null);
        if (_display == IntPtr.Zero)
            throw new InvalidOperationException("XOpenDisplay failed; no global hotkey.");

        _root = XDefaultRootWindow(_display);
        int keysym = KeysymForKey(hotkey.Key);
        _keycode = XKeysymToKeycode(_display, (nuint)keysym);
        _modifiers = MaskFor(hotkey.Modifiers);

        // Grab the combo, and the same combo with CapsLock / NumLock also held,
        // so the hotkey still fires when those locks are on.
        foreach (uint extra in new uint[] { 0, LockMask, Mod2Mask, LockMask | Mod2Mask })
            XGrabKey(_display, _keycode, _modifiers | extra, _root, true, GrabModeAsync, GrabModeAsync);
        XFlush(_display);

        _thread = new Thread(Loop) { IsBackground = true, Name = "Overlay hotkey (X11)" };
        _thread.Start();
    }

    private void Loop()
    {
        byte[] ev = new byte[256];   // >= sizeof(XEvent)
        while (_running)
        {
            // Poll so shutdown is prompt; the grab is idle most of the time.
            while (_running && XPending(_display) > 0)
            {
                XNextEvent(_display, ev);
                int type = BitConverter.ToInt32(ev, 0);
                if (type == KeyPress)
                {
                    try { _onToggle(); }
                    catch (Exception e) { _log?.Invoke($"[hotkey] toggle failed: {e.Message}"); }
                }
            }
            Thread.Sleep(15);
        }
    }

    public void Dispose()
    {
        _running = false;
        _thread.Join(TimeSpan.FromSeconds(1));
        foreach (uint extra in new uint[] { 0, LockMask, Mod2Mask, LockMask | Mod2Mask })
            XUngrabKey(_display, _keycode, _modifiers | extra, _root);
        XCloseDisplay(_display);
    }

    private static int KeysymForKey(Key k)
    {
        if (k >= Key.F1 && k <= Key.F12) return 0xFFBE + (k - Key.F1);
        if (k >= Key.A && k <= Key.Z) return 0x61 + (k - Key.A);   // lowercase keysym
        if (k >= Key.D0 && k <= Key.D9) return 0x30 + (k - Key.D0);
        return k switch
        {
            Key.Space => 0x20,
            Key.Enter => 0xFF0D,
            Key.Escape => 0xFF1B,
            Key.Tab => 0xFF09,
            _ => 0,
        };
    }

    private static uint MaskFor(KeyModifiers m)
    {
        uint r = 0;
        if (m.HasFlag(KeyModifiers.Shift)) r |= ShiftMask;
        if (m.HasFlag(KeyModifiers.Control)) r |= ControlMask;
        if (m.HasFlag(KeyModifiers.Alt)) r |= Mod1Mask;
        if (m.HasFlag(KeyModifiers.Meta)) r |= Mod4Mask;
        return r;
    }

    [LibraryImport("libX11", StringMarshalling = StringMarshalling.Utf8)]
    private static partial IntPtr XOpenDisplay(string? name);

    [LibraryImport("libX11")]
    private static partial int XCloseDisplay(IntPtr display);

    [LibraryImport("libX11")]
    private static partial nuint XDefaultRootWindow(IntPtr display);

    [LibraryImport("libX11")]
    private static partial byte XKeysymToKeycode(IntPtr display, nuint keysym);

    [LibraryImport("libX11")]
    private static partial int XGrabKey(IntPtr display, int keycode, uint modifiers, nuint grabWindow,
                                        [MarshalAs(UnmanagedType.Bool)] bool ownerEvents, int pointerMode, int keyboardMode);

    [LibraryImport("libX11")]
    private static partial int XUngrabKey(IntPtr display, int keycode, uint modifiers, nuint grabWindow);

    [LibraryImport("libX11")]
    private static partial int XPending(IntPtr display);

    [LibraryImport("libX11")]
    private static partial int XNextEvent(IntPtr display, [Out] byte[] eventReturn);

    [LibraryImport("libX11")]
    private static partial int XFlush(IntPtr display);
}
