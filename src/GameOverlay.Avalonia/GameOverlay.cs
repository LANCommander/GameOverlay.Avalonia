using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Renders an Avalonia control tree as an interactive overlay composited into a
/// running game's own backbuffer — Direct3D 11/12, Vulkan or OpenGL, including
/// exclusive fullscreen.
/// </summary>
/// <remarks>
/// <para>
/// <b>Call from an initialised Avalonia application.</b> The overlay runs inside
/// the consumer's app on the ambient <see cref="Dispatcher.UIThread"/>; the
/// library never calls <c>AppBuilder</c> itself (a second one in the same
/// process would throw). An overlay-only app should set up a minimal Avalonia
/// application first — see the sample.
/// </para>
/// <para>
/// The factory methods return promptly. Injection, the payload handshake and the
/// frame pump run on a background thread; <see cref="Attached"/> is raised on the
/// UI thread once the overlay is live, <see cref="GameExited"/> when it ends.
/// </para>
/// </remarks>
public sealed class GameOverlay : IDisposable
{
    private readonly IProcessInjector _game;
    private readonly GameOverlayOptions _options;
    private readonly Action<string>? _log;
    private readonly Action? _afterInject;
    private readonly OverlayHost _host;
    private readonly OverlaySession _session;
    private readonly CancellationTokenSource _cancellation = new();
    private readonly Thread _worker;

    private readonly object _hotkeyLock = new();
    private IGlobalHotkey? _hotkey;
    private OverlayHotkey? _toggleHotkey;

    private Control? _content;
    private volatile bool _disposed;

    private GameOverlay(IProcessInjector game, GameOverlayOptions options, Action? afterInject)
    {
        RequireAvalonia();

        _game = game;
        _options = options;
        _log = options.DiagnosticLog;
        _afterInject = afterInject;

        _host = OnUiThread(() => new OverlayHost(options.Scaling));
        _session = new OverlaySession(game, ResolvePayloadPath(options, game), _host, _log);
        _session.GameExited += OnGameExited;

        ToggleHotkey = options.ToggleHotkey;

        _worker = new Thread(RunWorker) { IsBackground = true, Name = "GameOverlay worker" };
        _worker.Start();
    }

    // --- factories -----------------------------------------------------------

    /// <summary>Attaches the overlay to an already-running game by process id.</summary>
    public static GameOverlay AttachToProcess(int processId, GameOverlayOptions? options = null)
    {
        options ??= new GameOverlayOptions();
        var game = PlatformServices.OpenProcess(processId, options.DiagnosticLog);
        return new GameOverlay(game, options, afterInject: null);
    }

    /// <summary>
    /// Attaches to a running game by process name (without <c>.exe</c>). Throws if
    /// zero or more than one process matches.
    /// </summary>
    public static GameOverlay AttachToProcess(string processName, GameOverlayOptions? options = null)
    {
        var matches = Process.GetProcessesByName(processName);
        if (matches.Length == 0)
            throw new InvalidOperationException($"No running process named '{processName}'.");
        if (matches.Length > 1)
            throw new InvalidOperationException(
                $"{matches.Length} processes named '{processName}'; attach by id instead.");
        return AttachToProcess(matches[0].Id, options);
    }

    /// <summary>
    /// Launches a game suspended, injects, then resumes it. Required for Vulkan
    /// (its device extensions can only be added at creation time) and safer for
    /// the other APIs too, as it removes the startup race.
    /// </summary>
    public static GameOverlay Launch(string exePath, string? arguments = null, GameOverlayOptions? options = null)
    {
        options ??= new GameOverlayOptions();
        var game = PlatformServices.LaunchSuspended(exePath, arguments, options.DiagnosticLog);
        return new GameOverlay(game, options, afterInject: game.ResumeMainThread);
    }

    // --- public surface ------------------------------------------------------

    /// <summary>The Avalonia control tree to project. May be set at any time.</summary>
    public Control? Content
    {
        get => _content;
        set { _content = value; _host.SetContent(value); }
    }

    /// <summary>
    /// Whether the overlay is shown and capturing input. While true the game
    /// receives no input; toggling it off gives input back.
    /// </summary>
    public bool Interactive
    {
        get => _session.Interactive;
        set => _session.Interactive = value;
    }

    /// <summary>Whether the overlay is composited at all (without capturing input).</summary>
    public bool Visible
    {
        get => _session.Visible;
        set => _session.Visible = value;
    }

    /// <summary>
    /// The global hotkey that toggles <see cref="Interactive"/>. Assign a new
    /// value to rebind it at runtime (for example from a settings screen), or
    /// <c>null</c> to remove the hotkey and drive <see cref="Interactive"/>
    /// yourself. Initialised from <see cref="GameOverlayOptions.ToggleHotkey"/>
    /// (Shift+F1 by default).
    /// </summary>
    public OverlayHotkey? ToggleHotkey
    {
        get { lock (_hotkeyLock) return _toggleHotkey; }
        set
        {
            lock (_hotkeyLock)
            {
                if (_disposed) return;
                _toggleHotkey = value;
                _hotkey?.Dispose();
                _hotkey = value is { } hotkey
                    ? PlatformServices.CreateHotkey(hotkey, () => Interactive = !Interactive, _log)
                    : null;
            }
        }
    }

    /// <summary>True once the payload handshake has completed.</summary>
    public bool IsAttached { get; private set; }

    /// <summary>True while the target game process is still running.</summary>
    public bool IsGameAlive => !_disposed && _session.IsGameAlive;

    /// <summary>The target game's process id.</summary>
    public int GameProcessId => _game.Pid;

    /// <summary>The game's graphics API. Valid once <see cref="Attached"/> has fired.</summary>
    public GameGraphicsApi GraphicsApi => _session.GraphicsApi;

    /// <summary>The overlay UI scale currently in effect.</summary>
    public double RenderScaling => _session.RenderScaling;

    /// <summary>Live diagnostic counters.</summary>
    public OverlayStatistics Statistics => _session.Statistics;

    /// <summary>Set if the overlay failed to attach.</summary>
    public Exception? LastError { get; private set; }

    /// <summary>Raised on the UI thread once the overlay is live.</summary>
    public event EventHandler? Attached;

    /// <summary>
    /// Raised on the UI thread when the overlay ends — the game exited, or attach
    /// failed (see <see cref="LastError"/>).
    /// </summary>
    public event EventHandler? GameExited;

    // --- worker + lifecycle --------------------------------------------------

    private void RunWorker()
    {
        try
        {
            _session.Attach(_options.AttachTimeout, _afterInject);
        }
        catch (Exception ex)
        {
            LastError = ex;
            _log?.Invoke($"[overlay] attach failed: {ex.Message}");
            RaiseOnUi(() => { GameExited?.Invoke(this, EventArgs.Empty); });
            return;
        }

        RaiseOnUi(() =>
        {
            IsAttached = true;
            if (_options.StartInteractive) Interactive = true;
            Attached?.Invoke(this, EventArgs.Empty);
        });

        // Blocks until the game exits or Dispose cancels; it raises the session's
        // GameExited on game exit, which is forwarded to the public event.
        _session.Run(_cancellation.Token, _options.TargetFps);
    }

    private void OnGameExited() => RaiseOnUi(() => { if (!_disposed) GameExited?.Invoke(this, EventArgs.Empty); });

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _cancellation.Cancel();
        _worker.Join(TimeSpan.FromSeconds(3));

        lock (_hotkeyLock) { _hotkey?.Dispose(); _hotkey = null; }
        _session.Dispose();   // releases capture, hides, detaches the payload
        _host.Dispose();
        _game.Dispose();
        _cancellation.Dispose();
    }

    // --- helpers -------------------------------------------------------------

    private static void RequireAvalonia()
    {
        if (Application.Current is null)
            throw new InvalidOperationException(
                "Avalonia is not initialised. Create a GameOverlay from within a running Avalonia " +
                "application (AppBuilder…SetupWithoutStarting()/Start()), on or with access to its UI thread.");
    }

    private static T OnUiThread<T>(Func<T> factory)
        => Dispatcher.UIThread.CheckAccess() ? factory() : Dispatcher.UIThread.Invoke(factory);

    private void RaiseOnUi(Action action) => Dispatcher.UIThread.Post(action);

    private static string ResolvePayloadPath(GameOverlayOptions options, IProcessInjector game)
    {
        if (!string.IsNullOrEmpty(options.PayloadPath))
        {
            if (!File.Exists(options.PayloadPath))
                throw new FileNotFoundException("Overlay payload not found", options.PayloadPath);
            return options.PayloadPath;
        }

        // Payload file name, runtime asset RID, and dev build tree all differ by
        // OS: a .dll under build/bin on Windows, a .so under build-linux/bin on
        // Linux.
        bool windows = OperatingSystem.IsWindows();
        string fileName = windows ? "GameOverlay.Native.dll" : "GameOverlay.Native.so";
        string rid = windows ? "win-x64" : "linux-x64";
        string devTree = windows ? "build" : "build-linux";

        // The payload must match the *target's* bitness, not the host's: the x64
        // host injects its x86 payload into 32-bit (WoW64) games. The x86 payload
        // rides alongside the x64 host under an "x86" subfolder (a win-x86 RID
        // asset would not deploy for a win-x64 app) and, in a dev tree, comes from
        // the parallel build-x86/bin. Everything else stays on the x64 layout.
        bool wantX86 = windows && game.Is32BitTarget;
        string sub = wantX86 ? "x86" : string.Empty;
        string devTreeForTarget = wantX86 ? devTree + "-x86" : devTree;

        string?[] candidates =
        {
            // NuGet native asset, and plain copy-to-output layouts.
            Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", sub, fileName),
            Path.Combine(AppContext.BaseDirectory, sub, fileName),
        };
        foreach (string? c in candidates)
            if (c is not null && File.Exists(c)) return c;

        // Dev convenience: walk up to a CMake build/bin (build-x86/bin,
        // build-linux/bin) tree.
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            string build = Path.Combine(dir.FullName, devTreeForTarget, "bin", fileName);
            if (File.Exists(build)) return build;
        }

        string ridPath = sub.Length == 0 ? $"runtimes/{rid}/native" : $"runtimes/{rid}/native/{sub}";
        throw new FileNotFoundException(
            $"{fileName} ({(wantX86 ? "x86" : "x64")}) was not found next to the application, under " +
            $"{ridPath}, or in a {devTreeForTarget}/bin tree. Set GameOverlayOptions.PayloadPath.");
    }
}
