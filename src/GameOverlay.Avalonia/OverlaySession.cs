using System;
using System.Diagnostics;
using System.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Ties together the game process, the injected payload and the frame source:
/// injects, waits for the handshake, tracks swapchain resizes, and pumps frames
/// until the game exits or we are asked to stop.
/// </summary>
/// <remarks>
/// This is the transport engine underneath the public <see cref="GameOverlay"/>.
/// It carries no UI and no console I/O of its own - operational diagnostics go
/// to the supplied log callback, which the library leaves null by default.
/// </remarks>
internal sealed class OverlaySession : IDisposable
{
    private readonly IProcessInjector _game;
    private readonly OverlaySharedState _state;
    private readonly string _payloadPath;
    private readonly IFrameSource _source;
    private readonly Action<string>? _log;

    private IFrameProducer? _producer;
    private InputRouter? _inputRouter;
    private uint _lastGeneration;
    private int _published;
    private int _skipped;

    public OverlaySession(IProcessInjector game, string payloadPath, IFrameSource source, Action<string>? log = null)
    {
        _game = game;
        _payloadPath = payloadPath;
        _source = source;
        _log = log;
        _state = OverlaySharedState.CreateOrOpen((uint)game.Pid);
    }

    private void Log(string message) => _log?.Invoke(message);

    // --- state the public wrapper reads --------------------------------------

    public GameGraphicsApi GraphicsApi => _state.GraphicsApi;
    public bool IsGameAlive => _game.IsAlive;
    public double RenderScaling => (_source as OverlayHost)?.RenderScaling ?? 1.0;
    public (int Width, int Height) GameSize => ((int)_state.GameWidth, (int)_state.GameHeight);

    /// <summary>Diagnostics snapshot, as the public <see cref="OverlayStatistics"/>.</summary>
    public OverlayStatistics Statistics
    {
        get
        {
            var r = _inputRouter?.Stats ?? default;
            return new OverlayStatistics
            {
                GamePresents = _state.PresentCount,
                PayloadDraws = _state.DrawCount,
                PayloadMutexTimeouts = _state.MutexTimeoutCount,
                InputSeen = _state.InputSeenCount,
                InputDispatched = (uint)r.Dispatched,
                InputDropped = r.Dropped,
                Published = (uint)Volatile.Read(ref _published),
                Skipped = (uint)Volatile.Read(ref _skipped),
            };
        }
    }

    /// <summary>Toggles whether the payload composites the overlay at all.</summary>
    public bool Visible
    {
        get => _state.Visible;
        set
        {
            _state.Visible = value;
            Log($"[session] overlay {(value ? "shown" : "hidden")}");
        }
    }

    /// <summary>
    /// Shows the overlay and takes input, or hides it and gives input back.
    /// </summary>
    /// <remarks>
    /// Visibility and interactivity are deliberately one switch. An overlay that
    /// is visible but swallowing input, or invisible but interactive, is just a
    /// way to lose someone's keystrokes.
    /// </remarks>
    public bool Interactive
    {
        get => _state.InputCapture;
        set
        {
            if (_state.InputCapture == value) return;

            // Order matters on the way out: stop capturing before hiding, so the
            // payload's WndProc sees capture end while it is still being told to
            // draw, and hover states get a chance to clear.
            if (value)
            {
                _state.Visible = true;
                _state.InputCapture = true;
            }
            else
            {
                _state.InputCapture = false;
                _inputRouter?.NotifyCaptureEnded();
            }

            (_source as OverlayHost)?.SetInteractive(value);
            Log($"[session] overlay {(value ? "interactive" : "passive")}");
        }
    }

    /// <summary>Raised (on the pump thread) when the game process exits.</summary>
    public event Action? GameExited;

    /// <summary>
    /// Injects the payload and blocks until it reports a usable swapchain.
    /// </summary>
    /// <param name="timeout">How long to wait for the payload handshake.</param>
    /// <param name="afterInject">
    /// Runs once the payload is loaded but before we start waiting. This is where
    /// a suspended game gets resumed: it must not run before the hooks are
    /// installed, and the handshake can never complete until it does.
    /// </param>
    public void Attach(TimeSpan timeout, Action? afterInject = null)
    {
        _state.HostPid = (uint)Environment.ProcessId;
        _state.Visible = true;

        _game.InjectPayload(_payloadPath);
        afterInject?.Invoke();

        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < timeout)
        {
            if (!_game.IsAlive)
                throw new InvalidOperationException("The game exited during attach.");

            // Three conditions, and all of them matter: a matching ABI proves we
            // are talking to a compatible payload, DllAttached proves the hooks
            // are live, and a non-zero size proves Present has run at least once
            // so the swapchain details are real.
            if (_state.DllAbiVersion == OverlaySharedState.AbiVersion &&
                _state.DllAttached &&
                _state.GameWidth > 0 && _state.GameHeight > 0)
            {
                Log($"[session] payload ready: {_state.GameWidth}x{_state.GameHeight}, " +
                    $"api={_state.GraphicsApi}, " +
                    $"adapter LUID 0x{_state.AdapterLuid:X16}, " +
                    $"srgb backbuffer={_state.BackbufferIsSrgb}");

                // The transport depends on the game's graphics API; the platform
                // backend picks it (D3D9 takes the CPU shared-memory path, the
                // rest share a GPU texture).
                _producer = PlatformServices.CreateFrameProducer(_state.GraphicsApi, _game, _state, _log);
                SyncSize();

                // Only an Avalonia host can consume input; a raw frame source
                // has nothing to route it to.
                if (_source is OverlayHost host)
                {
                    _inputRouter = new InputRouter(_state, host.TopLevel, PlatformServices.CreateKeyMapper(), _log);
                }
                return;
            }

            Thread.Sleep(25);
        }

        throw new TimeoutException(
            $"Payload did not report a swapchain within {timeout.TotalSeconds:0}s. " +
            $"abi={_state.DllAbiVersion} attached={_state.DllAttached} " +
            $"size={_state.GameWidth}x{_state.GameHeight} presents={_state.PresentCount}. " +
            "Check the payload log in %TEMP%\\GameOverlay.Avalonia.<pid>.log.");
    }

    private void SyncSize()
    {
        int width = (int)_state.GameWidth;
        int height = (int)_state.GameHeight;
        if (width <= 0 || height <= 0) return;

        _producer!.EnsureSize(width, height);
        _source.Resize(width, height);
        _lastGeneration = _state.SwapchainGeneration;
    }

    /// <summary>
    /// Frame pump. Runs at a fixed cadence independent of the game's frame rate -
    /// the overlay has no reason to redraw at 500 fps just because the game can.
    /// </summary>
    public void Run(CancellationToken cancellation, int targetFps = 60)
    {
        if (_producer is null) throw new InvalidOperationException("Attach must be called first.");

        var frameInterval = TimeSpan.FromSeconds(1.0 / targetFps);
        var clock = Stopwatch.StartNew();
        var statsClock = Stopwatch.StartNew();
        int published = 0, skipped = 0, mismatched = 0;

        while (!cancellation.IsCancellationRequested)
        {
            if (!_game.IsAlive)
            {
                Log("[session] game exited");
                GameExited?.Invoke();
                return;
            }

            // The payload bumps the generation on every ResizeBuffers, which
            // covers both window resizes and fullscreen mode transitions.
            if (_state.SwapchainGeneration != _lastGeneration)
            {
                Log($"[session] swapchain changed -> {_state.GameWidth}x{_state.GameHeight}");
                SyncSize();
            }

            var frameStart = clock.Elapsed;

            if (_source.TryBeginFrame(out IntPtr buffer, out int rowBytes, out int srcW, out int srcH))
            {
                try
                {
                    // A source may apply a resize asynchronously (Avalonia does -
                    // it is posted to the UI thread), so on a swapchain change the
                    // producer can be the new size while the source is still the
                    // old one. Publishing that mismatch reads past the end of the
                    // source buffer, so wait for the two to agree.
                    if (srcW != _producer.Width || srcH != _producer.Height)
                    {
                        mismatched++;
                    }
                    else if (_producer.TryPublishFrame(buffer, rowBytes))
                    {
                        published++;
                    }
                    else
                    {
                        skipped++;   // payload still reading the previous frame
                    }
                }
                finally
                {
                    _source.EndFrame();
                }
            }

            Volatile.Write(ref _published, published);
            Volatile.Write(ref _skipped, skipped);

            if (statsClock.Elapsed >= TimeSpan.FromSeconds(5))
            {
                string input = _inputRouter is { } router
                    ? $" input(seen={_state.InputSeenCount} pushed={_state.InputPushCount} " +
                      $"ring={_state.RingWriteIndex}/{_state.RingReadIndex} " +
                      $"drained={router.Stats.Drained} processed={router.Stats.Processed} " +
                      $"dispatched={router.Stats.Dispatched} dropped={router.Stats.Dropped})"
                    : string.Empty;

                Log($"[session] published={published} skipped={skipped} sizeMismatch={mismatched} " +
                    $"gamePresents={_state.PresentCount} payloadDraws={_state.DrawCount} " +
                    $"payloadMutexTimeouts={_state.MutexTimeoutCount}" + input);
                published = skipped = mismatched = 0;
                statsClock.Restart();
            }

            _state.Heartbeat();

            var remaining = frameInterval - (clock.Elapsed - frameStart);
            if (remaining > TimeSpan.Zero) Thread.Sleep(remaining);
        }
    }

    public void Dispose()
    {
        // Release input before anything else: leaving inputCapture set while we
        // tear down would leave the game unable to receive its own input.
        try { _state.InputCapture = false; } catch { /* mapping may already be gone */ }
        _inputRouter?.Dispose();

        // Hide before unhooking so the payload stops compositing immediately,
        // rather than leaving a stale frame on screen during teardown.
        try { _state.Visible = false; } catch { /* mapping may already be gone */ }
        Thread.Sleep(50);

        try { _game.DetachPayload(_payloadPath); }
        catch (Exception ex) { Log($"[session] detach failed: {ex.Message}"); }

        _producer?.Dispose();
        _state.Dispose();
    }
}
