using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Embedding;
using Avalonia.Input;
using Avalonia.LogicalTree;
using Avalonia.Media;
using Avalonia.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Hosts a consumer's Avalonia <see cref="Control"/> offscreen and exposes its
/// rasterised output as a frame source, plus the library-owned cursor.
/// </summary>
/// <remarks>
/// <para>
/// This runs inside the <em>consumer's</em> Avalonia application: it is created
/// and driven on the ambient <see cref="Dispatcher.UIThread"/> and never
/// initialises Avalonia itself (a second <c>AppBuilder</c> in one process would
/// throw). The compositor that ticks it is the consumer's; a static overlay
/// simply reuses its last frame, which the payload keeps drawing.
/// </para>
/// <para>
/// Construct on the UI thread. The pump-thread entry points
/// (<see cref="Resize"/>, <see cref="TryBeginFrame"/>, <see cref="EndFrame"/>)
/// are safe to call from the frame pump: <see cref="Resize"/> posts to the UI
/// thread and the framebuffer surface is already lock-guarded and
/// double-buffered.
/// </para>
/// </remarks>
internal sealed class OverlayHost : IFrameSource
{
    private readonly SharedTextureFramebufferSurface _surface = new();
    private readonly OverlayTopLevelImpl _topLevel;
    private readonly EmbeddableControlRoot _root;
    private readonly ContentControl _contentHost;
    private readonly OverlayCursor _cursor = new();
    private readonly double? _scalingOverride;

    private bool _renderingStarted;
    private bool _disposed;

    /// <summary>Must be called on the UI thread.</summary>
    public OverlayHost(double? scalingOverride)
    {
        _scalingOverride = scalingOverride;
        _topLevel = new OverlayTopLevelImpl(_surface);

        _contentHost = new ContentControl();

        _root = new EmbeddableControlRoot(_topLevel)
        {
            // FluentTheme gives the root an opaque themed background, which for a
            // window is correct and for an overlay paints the whole game out.
            // null means "paint no backdrop at all".
            Background = null,

            // The one that actually matters. OffscreenTopLevelImplBase reports
            // TransparencyLevel.None and seals the property, so Avalonia decides
            // the surface cannot do per-pixel alpha and fills it with
            // TransparencyBackgroundFallback - which defaults to solid *white*.
            // That is what would otherwise paint the game out.
            TransparencyLevelHint = new[] { global::Avalonia.Controls.WindowTransparencyLevel.Transparent },
            TransparencyBackgroundFallback = Brushes.Transparent,

            // Consumer content underneath, the library cursor on top.
            Content = new Panel { Children = { _contentHost, _cursor.Visual } },
        };

        _topLevel.CursorChanged += _cursor.SetShape;
        _root.Prepare();
        // StartRendering is deferred to the first real Resize, so we never render
        // a zero-sized frame.
    }

    /// <summary>The offscreen top level, for the input router.</summary>
    public OverlayTopLevelImpl TopLevel => _topLevel;

    public double RenderScaling => _topLevel.RenderScaling;

    /// <summary>The consumer's content control tree, applied on the UI thread.</summary>
    public void SetContent(Control? content)
        => Dispatcher.UIThread.Post(() => { if (!_disposed) _contentHost.Content = content; });

    /// <summary>
    /// The overlay's UI scale. The game's backbuffer is physical pixels and
    /// carries no DPI, so this is a presentation choice; deriving it from
    /// resolution keeps the UI a similar physical size from 720p to 4K, clamped
    /// so it does not swallow a very high-resolution screen.
    /// </summary>
    private double ScalingFor(int height)
        => _scalingOverride ?? Math.Clamp(height / 720.0 * 1.5, 1.0, 3.0);

    public void Resize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        Dispatcher.UIThread.Post(() =>
        {
            if (_disposed) return;
            _topLevel.SetSize(width, height, ScalingFor(height));
            if (!_renderingStarted)
            {
                _renderingStarted = true;
                _root.StartRendering();
            }
        }, DispatcherPriority.Send);
    }

    /// <summary>
    /// Shows/hides the cursor and moves focus into the overlay when it becomes
    /// interactive, so typing has somewhere to go.
    /// </summary>
    public void SetInteractive(bool interactive)
        => Dispatcher.UIThread.Post(() =>
        {
            if (_disposed) return;
            _cursor.SetVisible(interactive);
            if (interactive) FocusFirstControl();
        }, DispatcherPriority.Input);

    public void SetCursorPosition(Point position)
        => Dispatcher.UIThread.Post(() => { if (!_disposed) _cursor.SetPosition(position); },
                                    DispatcherPriority.Input);

    private void FocusFirstControl()
    {
        // Key and text events route to the focused element, so give the overlay
        // focus on capture. Best-effort: a consumer click still focuses whatever
        // was clicked.
        var focusable = _contentHost.GetLogicalDescendants()
            .OfType<Control>()
            .FirstOrDefault(c => c.Focusable && c.IsEffectivelyEnabled && c.IsVisible);
        focusable?.Focus();
    }

    public bool TryBeginFrame(out IntPtr buffer, out int rowBytes, out int width, out int height)
        => _surface.TryBeginRead(out buffer, out rowBytes, out width, out height);

    public void EndFrame() => _surface.EndRead();

    public void Dispose()
    {
        // Runs on whatever thread disposes the overlay; hop to the UI thread to
        // stop rendering, then release the surface.
        try
        {
            Dispatcher.UIThread.Invoke(() =>
            {
                _disposed = true;
                if (_renderingStarted) _root.StopRendering();
            }, DispatcherPriority.Send);
        }
        catch
        {
            // Dispatcher may already be gone during shutdown; teardown is
            // best-effort.
        }

        _surface.Dispose();
    }
}
