# Integrating a game overlay into your Avalonia app

`GameOverlay.Avalonia` lets you render an ordinary Avalonia control tree as an interactive overlay composited directly into a running game's backbuffer, across Direct3D 9/10/11/12, Vulkan and OpenGL, including exclusive fullscreen. There is no separate transparent window: the library injects a small native payload into the game, hooks its present/swap path, and draws your UI over the game's own frame, so the overlay survives fullscreen mode changes that a normal top-level window cannot.

This guide is for application developers who want to add an overlay to their own Avalonia app. You bring an Avalonia `Control`; the library does the rest.

## Contents

1. [Requirements & platform support](#requirements--platform-support)
2. [Install](#install)
3. [Quick start: a standalone overlay app](#quick-start-a-standalone-overlay-app)
4. [Adding an overlay to an existing Avalonia app](#adding-an-overlay-to-an-existing-avalonia-app)
5. [Designing the overlay UI](#designing-the-overlay-ui)
6. [Interactivity & the toggle hotkey](#interactivity--the-toggle-hotkey)
7. [Attaching vs. launching (and Vulkan)](#attaching-vs-launching-and-vulkan)
8. [API reference](#api-reference)
9. [Deployment](#deployment)
10. [Diagnostics & troubleshooting](#diagnostics--troubleshooting)
11. [Platform notes](#platform-notes)

## Requirements & platform support

- **.NET 10** and **Avalonia 11.3+**.
- **64-bit only.** The overlay targets x64 games; it will not inject into a 32-bit process.
- Your app must be a **running, initialized Avalonia application** (see below). The library never calls `AppBuilder` itself.

| Platform | Graphics APIs | Windowing | Notes |
|---|---|---|---|
| **Windows x64** | D3D9, D3D10, D3D11, D3D12, Vulkan, OpenGL | n/a | Attach to a running game or launch one. |
| **Linux x64** | OpenGL (GLX & desktop-GL EGL), Vulkan | X11 | Launch-only (the payload is preloaded at start). Wayland and OpenGL-ES are not yet supported. |
| **macOS** | none | none | Not supported. |

If the game runs elevated (as administrator/root), your host app must run elevated too, or injection will be denied.

## Install

Add the package (name it as published in your feed):

```xml
<PackageReference Include="GameOverlay.Avalonia" Version="0.1.0" />
```

The package carries the native payload as a runtime asset (`runtimes/win-x64/native/GameOverlay.Native.dll`, and on Linux `runtimes/linux-x64/native/GameOverlay.Native.so` plus the Vulkan layer). It deploys automatically when you publish with a matching **runtime identifier** (see [Deployment](#deployment)).

Target framework:

- Windows: `net10.0-windows`
- Linux: `net10.0`
- Cross-platform: `<TargetFrameworks>net10.0;net10.0-windows</TargetFrameworks>`

Your app also needs the usual Avalonia **platform support** (as any Avalonia app does) plus whatever **theme/fonts** your overlay UI uses, because the overlay library references only Avalonia core and Skia. The easiest way is the `Avalonia.Desktop` meta-package (Win32 + X11 + Skia); otherwise add `Avalonia.Win32` and/or `Avalonia.X11` directly. On Linux, `Avalonia.X11` is required (the overlay library does not pull it in for you).

## Quick start: a standalone overlay app

The simplest consumer is an app with **no window of its own** that just projects a control onto a game. You still initialize Avalonia (so there is a UI thread, dispatcher and renderer); you just never open a window.

**1. A minimal `Application`.** The library brings no themes or fonts, so include whatever your overlay UI needs:

```csharp
using Avalonia;
using Avalonia.Themes.Fluent;

public sealed class OverlayApp : Application
{
    public override void Initialize()
    {
        // Only needed if your overlay controls use themed styles.
        Styles.Add(new FluentTheme());
    }
}
```

**2. Boot Avalonia without a window, create the overlay, and pump the UI thread:**

```csharp
using System;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Threading;
using GameOverlay.Avalonia;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        BuildAvaloniaApp().SetupWithoutStarting();   // init Avalonia; no window

        var options = new GameOverlayOptions
        {
            ToggleHotkey     = new OverlayHotkey(Key.F1, KeyModifiers.Shift),
            StartInteractive = false,
            DiagnosticLog    = Console.WriteLine,
        };

        // Attach to a running game by name, or Launch(exePath) instead.
        using GameOverlay overlay = GameOverlay.AttachToProcess("MyGame", options);

        // Your overlay UI - any Avalonia control.
        overlay.Content = new Border
        {
            Background   = new SolidColorBrush(Color.FromArgb(160, 20, 24, 32)),
            CornerRadius = new CornerRadius(12),
            Padding      = new Thickness(16),
            Child        = new TextBlock { Text = "Hello from the overlay", Foreground = Brushes.White },
        };

        var cts = new CancellationTokenSource();
        overlay.GameExited += (_, _) => cts.Cancel();

        // Injection and the frame pump run on background threads; this just keeps
        // the Avalonia UI thread alive so your controls render.
        Dispatcher.UIThread.MainLoop(cts.Token);
    }

    private static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<OverlayApp>()
        .UsePlatformDetect()   // Win32 on Windows, X11 on Linux
        .UseSkia()
        .WithInterFont();      // the library brings no fonts; add one your UI can use
}
```

Notes:

- `SetupWithoutStarting()` initializes Avalonia on the calling (UI) thread and returns immediately. `Dispatcher.UIThread.MainLoop(token)` then runs the dispatcher with no window.
- `.WithInterFont()` needs the `Avalonia.Fonts.Inter` package; use any font your UI requires.
- The overlay never creates a window and Skia rasterizes into a CPU framebuffer, so on Windows you can skip GPU init entirely by booting with explicit software rendering instead of `UsePlatformDetect()`:

  ```csharp
  .UseWin32().With(new Win32PlatformOptions { RenderingMode = new[] { Win32RenderingMode.Software } })
  ```

- `AttachToProcess`, `Launch` and setting `Content` should be done **on the UI thread** (where you booted Avalonia).

## Adding an overlay to an existing Avalonia app

If your app already runs a normal Avalonia lifetime (it has windows and a running dispatcher), you do **not** boot Avalonia again and you do **not** run a second `MainLoop`. Just create the overlay on the UI thread and set its content, for example in response to a button or when a game is detected:

```csharp
private GameOverlay? _overlay;

private void StartOverlay(int gamePid)
{
    // On the UI thread (e.g. from a click handler).
    _overlay = GameOverlay.AttachToProcess(gamePid, new GameOverlayOptions
    {
        ToggleHotkey = new OverlayHotkey(Key.Tab, KeyModifiers.Alt),
    });
    _overlay.Content = new MyOverlayView();     // your UserControl
    _overlay.Attached  += (_, _) => { /* overlay is live */ };
    _overlay.GameExited += (_, _) => { _overlay?.Dispose(); _overlay = null; };
}
```

The overlay's own control tree is hosted in a separate embeddable root and runs on your app's ambient `Dispatcher.UIThread` and compositor. It is independent of your app's windows: closing your window does not close the overlay, and vice versa. Dispose the overlay when you are done (or on app shutdown).

You can run more than one overlay (for example targeting different games) from a single app; each `GameOverlay` instance is independent.

## Designing the overlay UI

The overlay is an ordinary Avalonia control tree, with a few things to keep in mind because it is composited over the game:

- **Keep backgrounds transparent or translucent.** The overlay surface is transparent by default. If your root control paints an **opaque** background it will cover the entire game frame. Use `null`, `Transparent`, or a semi-transparent brush for anything meant to sit over the game, and give solid backgrounds only to the specific panels or cards you want visible.
- **Alpha blends correctly.** Your UI is composited with premultiplied source-over blending, so translucent panels, drop shadows and anti-aliased text look as they do in a normal Avalonia window.
- **The overlay draws its own cursor.** In a captured game there is often no usable OS cursor (mouse-look games clip it to the screen centre, exclusive fullscreen may hide it), so while interactive the library renders a cursor for you. Design your hit-targets accordingly.
- **Sizing follows the game.** The overlay surface matches the game's backbuffer resolution and tracks resizes and fullscreen transitions automatically.
- **Scaling.** By default the UI scale is derived from the game's resolution so the UI stays a similar physical size from 720p to 4K. Override it with `GameOverlayOptions.Scaling`, and read the effective scale from `GameOverlay.RenderScaling`.

There is nothing overlay-specific about the controls themselves: bindings, `UserControl`s, animations and styles all work.

## Interactivity & the toggle hotkey

Visibility and input capture are deliberately **one switch**:

- **`Interactive = true`** shows the overlay **and** captures mouse/keyboard. While captured, the game receives no input and your controls do. This is how the user clicks buttons or types in the overlay.
- **`Interactive = false`** returns input to the game. (See `Visible` below if you want the overlay drawn but non-interactive.)

Drive it however you like:

```csharp
overlay.Interactive = !overlay.Interactive;   // toggle from your own UI/logic
```

Most apps let the user toggle it with a **global hotkey**, set via `GameOverlayOptions.ToggleHotkey` (default **Shift+F1**). The hotkey is a passive, host-side keyboard hook: it observes the combination without swallowing it, and nothing extra is injected into the game. Set `ToggleHotkey = null` to install no hotkey and drive `Interactive` yourself.

You can also **rebind the hotkey at runtime** through the `GameOverlay.ToggleHotkey` property, for example from a settings screen. Assigning re-registers the listener immediately; assigning `null` removes it:

```csharp
overlay.ToggleHotkey = new OverlayHotkey(Key.O, KeyModifiers.Control);  // Ctrl+O
overlay.ToggleHotkey = null;                                            // no hotkey
```

`Visible` is a lower-level switch: `Visible = true, Interactive = false` composites the overlay without capturing input (for example a passive HUD). Setting `Interactive` implies `Visible`.

## Attaching vs. launching (and Vulkan)

- **`AttachToProcess(int pid)` / `AttachToProcess(string name)`** injects into an already-running game. Attaching by name throws if zero or more than one process matches; use the pid overload to disambiguate. (Windows only for a running process; Linux is launch-only.)
- **`Launch(exePath, arguments)`** starts the game (suspended), injects, then resumes it.

**Vulkan requires `Launch`.** A Vulkan overlay has to add device extensions before the game creates its `VkDevice`, which cannot be done after the fact, so for Vulkan games you must start them through `Launch`. It is also the safer choice for the other APIs (no startup race). On **Linux the overlay is launch-only for every API**, because the payload is preloaded at process start.

```csharp
using var overlay = GameOverlay.Launch(@"C:\Games\MyGame\MyGame.exe", arguments: "--windowed", options);
```

## API reference

### `GameOverlay`

Factory methods (all take an optional `GameOverlayOptions`):

| Member | Description |
|---|---|
| `static GameOverlay AttachToProcess(int processId, GameOverlayOptions?)` | Attach to a running game by pid. |
| `static GameOverlay AttachToProcess(string processName, GameOverlayOptions?)` | Attach by process name (throws on 0 or >1 match). |
| `static GameOverlay Launch(string exePath, string? arguments, GameOverlayOptions?)` | Launch a game suspended, inject, resume. Required for Vulkan; the only option on Linux. |

Instance members:

| Member | Description |
|---|---|
| `Control? Content { get; set; }` | The Avalonia control tree to project. Settable at any time. |
| `bool Interactive { get; set; }` | Show and capture input (game goes deaf), or hide and release. |
| `bool Visible { get; set; }` | Composite the overlay without capturing input. |
| `OverlayHotkey? ToggleHotkey { get; set; }` | The global toggle hotkey. Assign to rebind at runtime, or `null` to remove it. |
| `bool IsAttached { get; }` | True once the payload handshake has completed. |
| `bool IsGameAlive { get; }` | True while the target process is still running. |
| `int GameProcessId { get; }` | The target game's pid. |
| `GameGraphicsApi GraphicsApi { get; }` | The game's graphics API (valid after `Attached`). |
| `double RenderScaling { get; }` | The overlay UI scale in effect. |
| `OverlayStatistics Statistics { get; }` | Live diagnostic counters. |
| `Exception? LastError { get; }` | Set if attaching failed. |
| `event EventHandler? Attached` | Raised on the UI thread once the overlay is live. |
| `event EventHandler? GameExited` | Raised on the UI thread when the game exits or attach fails. |
| `void Dispose()` | Release capture, hide, detach the payload, and clean up. |

### `GameOverlayOptions`

| Property | Default | Description |
|---|---|---|
| `double? Scaling` | `null` | Fixed UI scale; `null` derives it from the game resolution. |
| `int TargetFps` | `60` | Overlay redraw/upload cadence, independent of the game's frame rate. |
| `OverlayHotkey? ToggleHotkey` | `Shift+F1` | Global hotkey that toggles `Interactive`; `null` installs none. |
| `bool StartInteractive` | `false` | Whether the overlay starts visible and capturing input. |
| `string? PayloadPath` | `null` | Explicit path to the native payload; `null` auto-discovers it. |
| `TimeSpan AttachTimeout` | `20 s` | How long to wait for the payload handshake. |
| `Action<string>? DiagnosticLog` | `null` | Sink for operational diagnostics. The library writes nothing by default. |

### `OverlayHotkey`

```csharp
public readonly record struct OverlayHotkey(Key Key, KeyModifiers Modifiers = KeyModifiers.None);
// e.g. new OverlayHotkey(Key.F1, KeyModifiers.Shift)
```

### `GameGraphicsApi`

`Unknown, D3D9, D3D10, D3D11, D3D12, Vulkan, OpenGL`.

### `OverlayStatistics`

A snapshot of counters for diagnostics and health checks: `GamePresents`, `PayloadDraws`, `PayloadMutexTimeouts`, `Published`, `Skipped`, `InputSeen`, `InputDispatched`, `InputDropped`.

## Deployment

The native payload must sit next to your published app so it can be injected. It ships as a **RID-specific native runtime asset**, so publish with a runtime identifier:

```bash
dotnet publish -c Release -r win-x64      # Windows
dotnet publish -c Release -r linux-x64    # Linux
```

This places `GameOverlay.Native.dll` (Windows), or `GameOverlay.Native.so` plus the Vulkan layer (`libVkLayer_gameoverlay.so` and `VkLayer_gameoverlay.json`) on Linux, in the publish output, where the library discovers them automatically. Alternatively, set `GameOverlayOptions.PayloadPath` to point at the payload explicitly.

The library looks for the payload in this order: your `GameOverlayOptions.PayloadPath`; `runtimes/<rid>/native/` next to the app; the app's base directory; and, as a dev convenience, a `build/bin` (`build-linux/bin` on Linux) tree above the app.

## Diagnostics & troubleshooting

Set `GameOverlayOptions.DiagnosticLog` to capture the injection, handshake and publish/counter messages:

```csharp
new GameOverlayOptions { DiagnosticLog = msg => Log.Debug("overlay: {Msg}", msg) };
```

The injected payload also writes a log next to the temp dir: `%TEMP%\GameOverlay.Avalonia.<pid>.log` on Windows, `/tmp/GameOverlay.Avalonia.<pid>.log` on Linux.

Common issues:

| Symptom | Cause / fix |
|---|---|
| `InvalidOperationException: Avalonia is not initialised` | Create the overlay from within a running Avalonia app, on the UI thread. Call `AppBuilder…SetupWithoutStarting()` (or use your app's existing lifetime) first. |
| `NotSupportedException: Process … is 32-bit` | The overlay is x64-only; the target game must be 64-bit. |
| `Win32Exception … OpenProcess … (access denied)` | The game is elevated; run your host elevated too. |
| `FileNotFoundException: GameOverlay.Native.* not found` | Publish with `-r <rid>` so the native asset deploys, or set `PayloadPath`. |
| `TimeoutException: Payload did not report a swapchain` | The game had not presented a frame within `AttachTimeout`, or a security product blocked the injection. Check the payload log. |
| Overlay covers the whole game | Your root control has an opaque background; make it transparent or translucent (see [Designing the overlay UI](#designing-the-overlay-ui)). |
| Nothing appears | Confirm `IsAttached` fired and `Interactive`/`Visible` is set; check `Statistics.PayloadDraws` is advancing. |

If the host process crashes, the payload detects the stalled heartbeat, stops compositing and hands input back to the game, so a dead host never leaves the game frozen or deaf.

## Platform notes

**Windows.** Boot with `.UseWin32().UseSkia()` and software rendering. All of D3D9/10/11/12, Vulkan and OpenGL are supported, including exclusive fullscreen. Attach to a running game or `Launch` one (Vulkan requires `Launch`).

**Linux.** Boot with `.UseX11().UseSkia()` and include a font (the library brings none, for example `.WithInterFont()` from `Avalonia.Fonts.Inter`). OpenGL (GLX and desktop-GL EGL) and Vulkan are supported on X11, and the overlay is **launch-only** (the payload preloads via `LD_PRELOAD` and a Vulkan layer). Wayland and OpenGL-ES are not yet supported.

**macOS** is not supported.
