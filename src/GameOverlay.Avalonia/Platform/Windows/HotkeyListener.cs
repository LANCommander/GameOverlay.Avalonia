using System;
using System.Runtime.InteropServices;
using System.Threading;
using Avalonia.Input;
using Avalonia.Win32.Input;

namespace GameOverlay.Avalonia;

/// <summary>
/// Windows <see cref="IGlobalHotkey"/>: a global hotkey for toggling the overlay.
/// </summary>
/// <remarks>
/// A low-level keyboard hook runs entirely in the host process, so toggling the
/// overlay needs no extra code inside the game. That matters: every line we do
/// not inject is a line that cannot destabilise or slow down the game, and
/// keyboard interception inside a game process is the single most
/// anti-cheat-sensitive thing an overlay can do.
///
/// The hook is passive - it observes the key and never swallows it - so games
/// still receive the keystroke.
/// </remarks>
internal sealed class HotkeyListener : IGlobalHotkey
{
    private const int WH_KEYBOARD_LL = 13;
    private const int WM_KEYDOWN = 0x0100;
    private const int WM_SYSKEYDOWN = 0x0104;
    private const int VK_SHIFT = 0x10;
    private const int VK_CONTROL = 0x11;
    private const int VK_MENU = 0x12;   // Alt
    private const int VK_LWIN = 0x5B;
    private const int VK_RWIN = 0x5C;

    private delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    private readonly HookProc _proc;   // must outlive the hook, or the CLR collects it
    private readonly Action _onToggle;
    private readonly Action<string>? _log;
    private readonly int _virtualKey;
    private readonly KeyModifiers _modifiers;
    private readonly Thread _thread;
    private readonly ManualResetEventSlim _ready = new(false);

    private IntPtr _hook;
    private uint _threadId;

    public HotkeyListener(OverlayHotkey hotkey, Action onToggle, Action<string>? log = null)
    {
        _onToggle = onToggle;
        _log = log;
        // Map the Avalonia Key to a Win32 virtual key via the same helper the
        // input router uses, so the hotkey definition stays in Avalonia terms.
        _virtualKey = KeyInterop.VirtualKeyFromKey(hotkey.Key);
        _modifiers = hotkey.Modifiers;
        _proc = HookCallback;

        // A low-level keyboard hook requires a thread with a message pump; the
        // OS delivers hook callbacks by dispatching to it.
        _thread = new Thread(ThreadMain) { IsBackground = true, Name = "Overlay hotkey" };
        _thread.Start();
        _ready.Wait(TimeSpan.FromSeconds(5));
    }

    private void Log(string message) => _log?.Invoke(message);

    private bool ModifiersHeld()
    {
        static bool Down(int vk) => (GetKeyState(vk) & 0x8000) != 0;
        if (_modifiers.HasFlag(KeyModifiers.Shift) != Down(VK_SHIFT)) return false;
        if (_modifiers.HasFlag(KeyModifiers.Control) != Down(VK_CONTROL)) return false;
        if (_modifiers.HasFlag(KeyModifiers.Alt) != Down(VK_MENU)) return false;
        if (_modifiers.HasFlag(KeyModifiers.Meta) != (Down(VK_LWIN) || Down(VK_RWIN))) return false;
        return true;
    }

    private void ThreadMain()
    {
        _threadId = GetCurrentThreadId();
        _hook = SetWindowsHookEx(WH_KEYBOARD_LL, _proc, IntPtr.Zero, 0);
        _ready.Set();

        if (_hook == IntPtr.Zero)
        {
            Log($"[hotkey] SetWindowsHookEx failed: {Marshal.GetLastWin32Error()}");
            return;
        }

        while (GetMessage(out MSG msg, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }
    }

    private IntPtr HookCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
        {
            int vk = Marshal.ReadInt32(lParam);
            if (vk == _virtualKey && ModifiersHeld())
            {
                try { _onToggle(); }
                catch (Exception ex) { Log($"[hotkey] toggle failed: {ex.Message}"); }
            }
        }

        return CallNextHookEx(IntPtr.Zero, code, wParam, lParam);
    }

    public void Dispose()
    {
        if (_hook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }
        if (_threadId != 0) PostThreadMessage(_threadId, 0x0012 /* WM_QUIT */, IntPtr.Zero, IntPtr.Zero);
        _thread.Join(TimeSpan.FromSeconds(1));
        _ready.Dispose();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSG
    {
        public IntPtr Hwnd;
        public uint Message;
        public IntPtr WParam;
        public IntPtr LParam;
        public uint Time;
        public int X;
        public int Y;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int idHook, HookProc proc, IntPtr module, uint threadId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int vk);

    [DllImport("user32.dll")]
    private static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref MSG msg);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref MSG msg);

    [DllImport("user32.dll")]
    private static extern bool PostThreadMessage(uint threadId, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();
}
