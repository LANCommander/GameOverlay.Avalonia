using System;
using System.Collections.Generic;
using System.Threading;
using Avalonia;
using Avalonia.Input;
using Avalonia.Input.Raw;
using Avalonia.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Drains input the payload captured from the game and feeds it to Avalonia.
/// </summary>
/// <remarks>
/// Runs its own thread rather than piggy-backing on the 60 Hz frame pump:
/// mouse motion sampled at 60 Hz feels noticeably laggy, and pointer drags
/// need every intermediate move to arrive for capture to behave.
/// </remarks>
internal sealed class InputRouter : IDisposable
{
    // WHEEL_DELTA. Avalonia expresses wheel movement in notches, Windows in
    // 120ths of one.
    private const double WheelDelta = 120.0;

    private readonly OverlaySharedState _state;
    private readonly OverlayTopLevelImpl _topLevel;
    private readonly IKeyMapper _keyMapper;
    private readonly Action<string>? _log;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Thread _thread;

    private void Log(string message) => _log?.Invoke(message);

    // Virtual cursor in physical backbuffer pixels. The host owns this rather
    // than reading the OS cursor: a game with the pointer clipped to screen
    // centre for mouse-look has no useful OS cursor position, and in exclusive
    // fullscreen there may be no visible system cursor at all.
    private double _cursorX;
    private double _cursorY;
    private bool _cursorInitialised;

    private int _drained;
    private int _processed;
    private int _dispatched;
    private bool _loggedNoRoot;
    private bool _loggedFailure;

    /// <summary>Diagnostics: events taken off the ring vs reaching the UI thread vs delivered.</summary>
    public (int Drained, int Processed, int Dispatched, uint Dropped) Stats
        => (Volatile.Read(ref _drained), Volatile.Read(ref _processed),
            Volatile.Read(ref _dispatched), _state.InputDropped);

    public InputRouter(OverlaySharedState state, OverlayTopLevelImpl topLevel, IKeyMapper keyMapper, Action<string>? log = null)
    {
        _state = state;
        _topLevel = topLevel;
        _keyMapper = keyMapper;
        _log = log;

        // Resolve every reflective binding now, so an incompatible Avalonia
        // fails loudly at attach rather than at the first click. This includes
        // the mouse device, whose factory is only exercised once input starts
        // flowing and so stayed silently broken while the overlay was
        // display-only.
        RawEventFactory.Validate();
        _ = topLevel.MouseDevice;

        _thread = new Thread(DrainLoop)
        {
            IsBackground = true,
            Name = "Overlay input drain",
        };
        _thread.Start();
    }

    /// <summary>Virtual cursor position in device-independent pixels.</summary>
    public Point CursorPosition => new(_cursorX / _topLevel.RenderScaling,
                                       _cursorY / _topLevel.RenderScaling);

    private void DrainLoop()
    {
        var batch = new List<SharedInputEvent>(64);

        while (!_shutdown.IsCancellationRequested)
        {
            batch.Clear();
            while (batch.Count < 256 && _state.TryDequeueInput(out SharedInputEvent evt))
            {
                batch.Add(evt);
            }

            if (batch.Count > 0)
            {
                Interlocked.Add(ref _drained, batch.Count);

                // Copy: the list is reused for the next drain, and the post is
                // asynchronous.
                var events = batch.ToArray();
                Dispatcher.UIThread.Post(() =>
                {
                    // An exception here would otherwise vanish into the
                    // dispatcher and look identical to "the post never ran".
                    try { Process(events); }
                    catch (Exception ex) when (!_loggedFailure)
                    {
                        _loggedFailure = true;
                        Log($"[input] dispatch failed: {ex}");
                    }
                }, DispatcherPriority.Input);
            }

            Thread.Sleep(2);
        }
    }

    private void Process(SharedInputEvent[] events)
    {
        Interlocked.Add(ref _processed, events.Length);

        IInputRoot? root = _topLevel.InputRoot;
        IInputDevice? pointer = _topLevel.MouseDevice;
        if (root is null || pointer is null)
        {
            if (!_loggedNoRoot)
            {
                _loggedNoRoot = true;
                Log($"[input] dropping events: inputRoot={(root is null ? "null" : "ok")} " +
                                  $"mouseDevice={(pointer is null ? "null" : "ok")}");
            }
            return;
        }

        foreach (SharedInputEvent evt in events)
        {
            RawInputModifiers modifiers = Translate(evt.Modifiers);

            switch (evt.Type)
            {
                case InputEventType.MouseMove:
                    SetCursor(evt.X, evt.Y);
                    Dispatch(RawEventFactory.Pointer(pointer, root, RawPointerEventType.Move, CursorPosition, modifiers));
                    break;

                case InputEventType.MouseMoveDelta:
                    MoveCursor(evt.X, evt.Y);
                    Dispatch(RawEventFactory.Pointer(pointer, root, RawPointerEventType.Move, CursorPosition, modifiers));
                    break;

                case InputEventType.MouseDown:
                case InputEventType.MouseUp:
                    {
                        SetCursor(evt.X, evt.Y);
                        bool down = evt.Type == InputEventType.MouseDown;
                        RawPointerEventType? type = (SharedMouseButton)evt.Data switch
                        {
                            SharedMouseButton.Left => down ? RawPointerEventType.LeftButtonDown : RawPointerEventType.LeftButtonUp,
                            SharedMouseButton.Right => down ? RawPointerEventType.RightButtonDown : RawPointerEventType.RightButtonUp,
                            SharedMouseButton.Middle => down ? RawPointerEventType.MiddleButtonDown : RawPointerEventType.MiddleButtonUp,
                            SharedMouseButton.X1 => down ? RawPointerEventType.XButton1Down : RawPointerEventType.XButton1Up,
                            SharedMouseButton.X2 => down ? RawPointerEventType.XButton2Down : RawPointerEventType.XButton2Up,
                            _ => null,
                        };
                        if (type is { } t)
                            Dispatch(RawEventFactory.Pointer(pointer, root, t, CursorPosition, modifiers));
                        break;
                    }

                case InputEventType.MouseWheel:
                case InputEventType.MouseHWheel:
                    {
                        SetCursor(evt.X, evt.Y);
                        // Data was written as a signed short widened through uint.
                        double notches = unchecked((short)evt.Data) / WheelDelta;
                        Vector delta = evt.Type == InputEventType.MouseWheel
                            ? new Vector(0, notches)
                            : new Vector(notches, 0);
                        Dispatch(RawEventFactory.Wheel(pointer, root, CursorPosition, delta, modifiers));
                        break;
                    }

                case InputEventType.KeyDown:
                case InputEventType.KeyUp:
                    {
                        // The key mapper needs the auxiliary key data (the Win32
                        // lParam scan-code bits) as well as the virtual key to
                        // tell left/right modifiers and the numpad apart.
                        Key key = _keyMapper.KeyFromVirtualKey((int)evt.Data, evt.Y);
                        if (key == Key.None) break;

                        RawKeyEventType type = evt.Type == InputEventType.KeyDown
                            ? RawKeyEventType.KeyDown
                            : RawKeyEventType.KeyUp;
                        Dispatch(RawEventFactory.Key(root, type, key, modifiers));
                        break;
                    }

                case InputEventType.Char:
                    {
                        char c = (char)evt.Data;
                        // Control characters arrive as WM_CHAR too; letting them
                        // through would insert a glyph for backspace and enter.
                        if (c >= ' ' || c == '\t')
                            Dispatch(RawEventFactory.Text(root, c.ToString()));
                        break;
                    }
            }
        }
    }

    private void Dispatch(RawInputEventArgs args)
    {
        Interlocked.Increment(ref _dispatched);
        _topLevel.DispatchInput(args);
    }

    private void SetCursor(int physicalX, int physicalY)
    {
        _cursorX = physicalX;
        _cursorY = physicalY;
        _cursorInitialised = true;
        ClampCursor();
    }

    private void MoveCursor(int deltaX, int deltaY)
    {
        if (!_cursorInitialised)
        {
            // No absolute position seen yet (a game that captured the pointer
            // before the overlay opened); start from the middle.
            _cursorX = _state.GameWidth / 2.0;
            _cursorY = _state.GameHeight / 2.0;
            _cursorInitialised = true;
        }

        _cursorX += deltaX;
        _cursorY += deltaY;
        ClampCursor();
    }

    private void ClampCursor()
    {
        double maxX = Math.Max(0, _state.GameWidth - 1);
        double maxY = Math.Max(0, _state.GameHeight - 1);
        _cursorX = Math.Clamp(_cursorX, 0, maxX);
        _cursorY = Math.Clamp(_cursorY, 0, maxY);
    }

    /// <summary>
    /// Tells Avalonia the pointer has left, so hover and pressed visuals do not
    /// stick when capture ends.
    /// </summary>
    public void NotifyCaptureEnded()
    {
        IInputRoot? root = _topLevel.InputRoot;
        IInputDevice? pointer = _topLevel.MouseDevice;
        if (root is null || pointer is null) return;

        Dispatcher.UIThread.Post(() =>
            Dispatch(RawEventFactory.Pointer(pointer, root, RawPointerEventType.LeaveWindow,
                                             CursorPosition, RawInputModifiers.None)),
            DispatcherPriority.Input);
    }

    private static RawInputModifiers Translate(SharedModifiers modifiers)
    {
        var result = RawInputModifiers.None;
        if (modifiers.HasFlag(SharedModifiers.Shift)) result |= RawInputModifiers.Shift;
        if (modifiers.HasFlag(SharedModifiers.Control)) result |= RawInputModifiers.Control;
        if (modifiers.HasFlag(SharedModifiers.Alt)) result |= RawInputModifiers.Alt;
        if (modifiers.HasFlag(SharedModifiers.LeftButton)) result |= RawInputModifiers.LeftMouseButton;
        if (modifiers.HasFlag(SharedModifiers.RightButton)) result |= RawInputModifiers.RightMouseButton;
        if (modifiers.HasFlag(SharedModifiers.MiddleButton)) result |= RawInputModifiers.MiddleMouseButton;
        return result;
    }

    public void Dispose()
    {
        _shutdown.Cancel();
        _thread.Join(TimeSpan.FromSeconds(1));
        _shutdown.Dispose();
    }
}
