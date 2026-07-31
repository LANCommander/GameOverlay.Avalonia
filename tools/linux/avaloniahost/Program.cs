using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using GameOverlay.Avalonia;

namespace GameOverlay.Linux.AvaloniaHost;

// Boots a minimal Avalonia app on X11 (headless under Xvfb), renders a real
// control tree through the overlay onto a GLX game, and reports the transport
// stats. With --inject <xtest_inject> it additionally turns the overlay
// interactive, fires synthetic input, and confirms it routes all the way into a
// focused Avalonia control - the full input loop, not just the raw ring.
internal sealed class App : Application
{
    public override void Initialize() { }
}

internal static class Program
{
    private static volatile int s_keyDowns;
    private static TextBox? s_textBox;
    private static string? s_hotkeyInjector;

    private static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: AvaloniaHost <game-exe> [seconds] [--inject <path>] [game-args...]");
            return 2;
        }

        string gameExe = args[0];
        int seconds = args.Length > 1 && int.TryParse(args[1], out int s) ? s : 6;

        string? injector = null;
        int injIdx = Array.IndexOf(args, "--inject");
        if (injIdx >= 0 && injIdx + 1 < args.Length) injector = args[injIdx + 1];

        int hkIdx = Array.IndexOf(args, "--hotkey");
        if (hkIdx >= 0 && hkIdx + 1 < args.Length) s_hotkeyInjector = args[hkIdx + 1];

        // Anything after the recognised args is forwarded to the game.
        string? gameArgs = args.Length > 2 && injIdx < 0
            ? string.Join(' ', args[2..])
            : (injIdx >= 0 && injIdx + 2 < args.Length ? string.Join(' ', args[(injIdx + 2)..]) : null);

        AppBuilder.Configure<App>().UseX11().UseSkia().WithInterFont().SetupWithoutStarting();

        GameOverlay.Avalonia.GameOverlay? overlay = null;

        Dispatcher.UIThread.Post(() =>
        {
            overlay = GameOverlay.Avalonia.GameOverlay.Launch(gameExe, gameArgs, new GameOverlayOptions
            {
                // Hotkey mode uses the default Shift+F1 toggle; other modes install none.
                ToggleHotkey = s_hotkeyInjector is not null ? new OverlayHotkey(Key.F1, KeyModifiers.Shift) : null,
                StartInteractive = false,
                DiagnosticLog = Console.WriteLine,
            });
            GameOverlayHolder.Overlay = overlay;

            s_textBox = new TextBox { Width = 360, FontSize = 18, Watermark = "focus + type here" };
            s_textBox.KeyDown += (_, _) => s_keyDowns++;

            overlay.Content = new Border
            {
                Width = 440, Height = 240,
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(48),
                CornerRadius = new CornerRadius(16),
                Background = new SolidColorBrush(Color.FromArgb(180, 24, 28, 40)),
                BorderBrush = new SolidColorBrush(Color.FromArgb(255, 90, 170, 255)),
                BorderThickness = new Thickness(2),
                Child = new StackPanel
                {
                    Margin = new Thickness(20), Spacing = 12,
                    Children =
                    {
                        new Ellipse { Width = 36, Height = 36, Fill = Brushes.OrangeRed },
                        new TextBlock { Text = "Avalonia overlay on Linux", FontSize = 20, Foreground = Brushes.White },
                        s_textBox,
                    },
                },
            };

            overlay.Visible = true;
        });

        // Avalonia's UI thread is this (main) thread, so MainLoop must run here.
        // In inject mode a worker drives the test via posts to the UI thread and
        // then cancels the loop; in display mode a timer cancels it.
        var stop = new CancellationTokenSource();
        int result = 1;

        if (injector is not null || s_hotkeyInjector is not null)
        {
            var worker = new Thread(() =>
            {
                result = injector is not null ? RunInputTest(injector) : RunHotkeyTest(s_hotkeyInjector!);
                stop.Cancel();
            }) { IsBackground = true, Name = "driver" };
            worker.Start();
        }
        else
        {
            stop.CancelAfter(TimeSpan.FromSeconds(seconds));
        }

        Dispatcher.UIThread.MainLoop(stop.Token);

        if (injector is null && s_hotkeyInjector is null) result = ReportDisplay(overlay);
        return result;
    }

    // Runs on a worker thread; touches Avalonia only through Dispatcher posts.
    private static int RunInputTest(string injector)
    {
        // Give attach + first render time, then go interactive and focus the box.
        Thread.Sleep(TimeSpan.FromSeconds(3));
        Dispatcher.UIThread.Invoke(() =>
        {
            GameOverlayHolder.Overlay!.Interactive = true;   // payload grabs input
            s_textBox!.Focus();
        });
        Thread.Sleep(TimeSpan.FromMilliseconds(800));

        int keysBefore = s_keyDowns;
        var inj = Process.Start(new ProcessStartInfo { FileName = injector, UseShellExecute = false });
        inj!.WaitForExit();
        Thread.Sleep(TimeSpan.FromSeconds(1));

        string text = Dispatcher.UIThread.Invoke(() => s_textBox!.Text ?? string.Empty);
        int keysAfter = s_keyDowns;
        Dispatcher.UIThread.Invoke(() => GameOverlayHolder.Overlay!.Interactive = false);

        Console.WriteLine($"[host] keyDowns before={keysBefore} after={keysAfter} textBox='{text}'");
        bool pass = keysAfter > keysBefore && text.Length > 0;
        Console.WriteLine(pass ? "RESULT: PASS" : "RESULT: FAIL");
        return pass ? 0 : 1;
    }

    // Verifies the X11 global hotkey toggles the overlay: fire Shift+F1 and
    // confirm Interactive flips false -> true.
    private static int RunHotkeyTest(string injector)
    {
        Thread.Sleep(TimeSpan.FromSeconds(3));

        // Rebind the toggle hotkey at runtime (default was Shift+F1) to Shift+F2,
        // then fire Shift+F2: a toggle proves the new binding took effect live.
        Dispatcher.UIThread.Invoke(() =>
            GameOverlayHolder.Overlay!.ToggleHotkey = new OverlayHotkey(Key.F2, KeyModifiers.Shift));
        Thread.Sleep(TimeSpan.FromMilliseconds(300));

        bool before = Dispatcher.UIThread.Invoke(() => GameOverlayHolder.Overlay!.Interactive);

        var inj = Process.Start(new ProcessStartInfo(injector, "F2") { UseShellExecute = false });
        inj!.WaitForExit();
        Thread.Sleep(TimeSpan.FromMilliseconds(800));

        bool after = Dispatcher.UIThread.Invoke(() => GameOverlayHolder.Overlay!.Interactive);
        Console.WriteLine($"[host] hotkey (rebound to Shift+F2): interactive before={before} after={after}");
        bool pass = !before && after;
        Console.WriteLine(pass ? "RESULT: PASS" : "RESULT: FAIL");
        return pass ? 0 : 1;
    }

    private static int ReportDisplay(GameOverlay.Avalonia.GameOverlay? overlay)
    {
        if (overlay is null) { Console.WriteLine("RESULT: FAIL"); return 1; }
        var st = overlay.Statistics;
        Console.WriteLine($"[host] stats: published={st.Published} payloadDraws={st.PayloadDraws} " +
                          $"api={overlay.GraphicsApi} attached={overlay.IsAttached}");
        bool pass = overlay.IsAttached && st.PayloadDraws > 0;
        Console.WriteLine(pass ? "RESULT: PASS" : "RESULT: FAIL");
        return pass ? 0 : 1;
    }
}

// Small holder so the input test can reach the overlay created on the UI thread.
internal static class GameOverlayHolder
{
    public static GameOverlay.Avalonia.GameOverlay? Overlay;
}
