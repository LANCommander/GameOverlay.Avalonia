using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Avalonia;
using Avalonia.Input;
using Avalonia.Threading;

namespace GameOverlay.Avalonia.Sample;

/// <summary>
/// Sample app and verification harness for the <c>GameOverlay.Avalonia</c>
/// library. It is a minimal Avalonia application (no visible window of its own)
/// that projects an <see cref="OverlayView"/> onto a game via the public API.
/// The CLI exists so the tools under <c>tools/</c> can drive it.
/// </summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length == 0 || args.Contains("--help") || args.Contains("-h"))
        {
            PrintUsage();
            return 1;
        }

        try
        {
            return args.Contains("--test-pattern") ? RunTestPattern(args) : RunOverlay(args);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine($"error: {ex.Message}");
            if (Environment.GetEnvironmentVariable("OVERLAY_TRACE") == "1")
                Console.Error.WriteLine(ex);
            return 1;
        }
    }

    // -----------------------------------------------------------------------
    // Normal path: the public library API projecting an Avalonia control tree.
    // -----------------------------------------------------------------------
    private static int RunOverlay(string[] args)
    {
        // Boot a minimal Avalonia application on this (the UI) thread. This is
        // the consumer's job, not the library's - the library never calls
        // AppBuilder. An overlay-only app just needs Avalonia initialised and a
        // dispatcher to run; there is no window.
        BuildAvaloniaApp().SetupWithoutStarting();

        using var cancellation = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };

        var options = new GameOverlayOptions
        {
            DiagnosticLog = Console.WriteLine,
            StartInteractive = args.Contains("--interactive"),
            ToggleHotkey = new OverlayHotkey(Key.F1, KeyModifiers.Shift),
            PayloadPath = GetOption(args, "--payload"),
        };

        GameOverlay overlay = GetOption(args, "--launch") is { } launchPath
            ? GameOverlay.Launch(launchPath, GetOption(args, "--launch-args"), options)
            : GameOverlay.AttachToProcess(ResolveTargetPid(args), options);

        // The projected control tree - an ordinary Avalonia UserControl. The
        // library composites it and draws the cursor.
        var view = new OverlayView();
        overlay.Content = view;

        Console.WriteLine($"[host] target pid {overlay.GameProcessId}");
        Console.WriteLine("[host] running - Shift+F1 toggles interaction, Ctrl+C detaches");

        // Republish where the demo controls laid out so the interaction test can
        // aim at them; recomputed on a timer so a resize (which rescales the UI)
        // is picked up.
        var targetsTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        targetsTimer.Tick += (_, _) => PrintTargets(view, overlay.RenderScaling);
        targetsTimer.Start();

        overlay.GameExited += (_, _) => cancellation.Cancel();

        // Pump the UI thread until the game exits or Ctrl+C. Injection and the
        // frame pump run on the overlay's own background threads.
        Dispatcher.UIThread.MainLoop(cancellation.Token);

        Console.WriteLine("[host] detaching");
        overlay.Dispose();
        return 0;
    }

    private static void PrintTargets(OverlayView view, double scaling)
    {
        var sb = new StringBuilder();
        foreach ((string name, Rect bounds) in view.GetTestTargets())
        {
            sb.AppendLine($"[target] {name}={(int)(bounds.X * scaling)},{(int)(bounds.Y * scaling)}," +
                          $"{(int)(bounds.Width * scaling)},{(int)(bounds.Height * scaling)}");
        }
        if (sb.Length > 0) Console.Write(sb.ToString());
    }

    // -----------------------------------------------------------------------
    // Diagnostic path: raw test pattern through the internal engine, to isolate
    // the transport from Avalonia. Uses the library's internals (friend access).
    // -----------------------------------------------------------------------
    private static int RunTestPattern(string[] args)
    {
        string payload = ResolvePayload(GetOption(args, "--payload"));
        int fps = int.TryParse(GetOption(args, "--fps"), out int f) ? f : 60;

        using GameProcess game = GetOption(args, "--launch") is { } launchPath
            ? GameProcess.LaunchSuspended(launchPath, GetOption(args, "--launch-args"), Console.WriteLine)
            : GameProcess.Open(ResolveTargetPid(args), Console.WriteLine);

        Console.WriteLine($"[host] target pid {game.Pid}");

        using var source = new TestPatternFrameSource();
        using var session = new OverlaySession(game, payload, source, Console.WriteLine);
        using var cancellation = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };

        session.Attach(TimeSpan.FromSeconds(20), afterInject: game.ResumeMainThread);
        using var hotkey = new HotkeyListener(new OverlayHotkey(Key.F1, KeyModifiers.Shift),
                                              () => session.Visible = !session.Visible, Console.WriteLine);

        Console.WriteLine("[host] running - Shift+F1 toggles the overlay, Ctrl+C detaches");
        session.Run(cancellation.Token, fps);

        Console.WriteLine("[host] detaching");
        return 0;
    }

    // -----------------------------------------------------------------------

    private static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<OverlayApp>()
            .UseWin32()
            .UseSkia()
            .With(new Win32PlatformOptions
            {
                // The overlay never creates an HWND and Skia rasterises into the
                // library's CPU framebuffer, so ANGLE/EGL would only add startup
                // cost and another failure mode.
                RenderingMode = new[] { Win32RenderingMode.Software },
            });

    private static int ResolveTargetPid(string[] args)
    {
        if (GetOption(args, "--pid") is { } pidText)
        {
            if (!int.TryParse(pidText, out int pid))
                throw new ArgumentException($"'{pidText}' is not a valid pid.");
            return pid;
        }

        if (GetOption(args, "--name") is { } name)
        {
            var matches = Process.GetProcessesByName(name);
            if (matches.Length == 0) throw new InvalidOperationException($"No running process named '{name}'.");
            if (matches.Length > 1)
                throw new InvalidOperationException($"{matches.Length} processes named '{name}'; use --pid.");
            return matches[0].Id;
        }

        throw new ArgumentException("Specify one of --pid, --name or --launch.");
    }

    private static string ResolvePayload(string? overridePath)
    {
        if (!string.IsNullOrEmpty(overridePath))
        {
            if (!File.Exists(overridePath)) throw new FileNotFoundException("Payload not found", overridePath);
            return overridePath;
        }
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            foreach (string candidate in new[]
            {
                Path.Combine(dir.FullName, "build", "bin", "GameOverlay.Native.dll"),
                Path.Combine(dir.FullName, "GameOverlay.Native.dll"),
            })
                if (File.Exists(candidate)) return candidate;
        }
        throw new FileNotFoundException("GameOverlay.Native.dll not found; pass --payload <path>.");
    }

    private static string? GetOption(string[] args, string name)
    {
        int index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("""
            Avalonia game overlay sample

            Usage:
              GameOverlay.Avalonia.Sample --pid <pid>            attach to a running game
              GameOverlay.Avalonia.Sample --name <exe-name>      attach by process name
              GameOverlay.Avalonia.Sample --launch <path.exe>    start a game and attach (required for Vulkan)

            Options:
              --payload <path>    override the injected DLL location
              --test-pattern      raw transport diagnostic (no Avalonia UI)
              --interactive       start with input capture on (default: Shift+F1 to toggle)
              --launch-args <s>   arguments for the launched game
              --fps <n>           overlay redraw rate (default 60)
            """);
    }
}
