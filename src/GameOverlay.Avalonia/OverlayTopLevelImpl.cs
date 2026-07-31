using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using Avalonia;
using Avalonia.Controls.Embedding.Offscreen;
using Avalonia.Controls.Platform.Surfaces;
using Avalonia.Input;
using Avalonia.Input.Raw;
using Avalonia.Platform;

namespace GameOverlay.Avalonia;

/// <summary>
/// An Avalonia top level that renders into a plain CPU buffer instead of a window.
/// </summary>
/// <remarks>
/// <para>
/// <c>ITopLevelImpl</c> cannot be implemented from outside Avalonia's own
/// assembly: it declares an abstract method whose name is the literal string
/// "(This interface or abstract class is -not- implementable by user code !)"
/// with <c>internal</c> accessibility, so no external type can satisfy it.
/// </para>
/// <para>
/// <see cref="OffscreenTopLevelImplBase"/> is the sanctioned way in. It is
/// public and abstract, implements the interface itself, and leaves only
/// <see cref="Surfaces"/> and <see cref="MouseDevice"/> for us - which is
/// exactly the seam an overlay needs.
/// </para>
/// </remarks>
internal sealed class OverlayTopLevelImpl : OffscreenTopLevelImplBase
{
    private readonly SharedTextureFramebufferSurface _surface;
    private readonly Lazy<IMouseDevice> _mouseDevice = new(CreateMouseDevice);

    public OverlayTopLevelImpl(SharedTextureFramebufferSurface surface)
    {
        _surface = surface;
    }

    public override IEnumerable<object> Surfaces => new object[] { _surface };

    public override IMouseDevice MouseDevice => _mouseDevice.Value;

    /// <summary>
    /// Delivers a raw input event to Avalonia.
    /// </summary>
    /// <remarks>
    /// <see cref="OffscreenTopLevelImplBase.Input"/> is the handler that
    /// <c>TopLevel</c> assigns and the platform invokes - the platform being us.
    /// It must only ever be invoked, never assigned, or Avalonia's own handler
    /// is unhooked and all input silently stops working.
    /// </remarks>
    public void DispatchInput(RawInputEventArgs args) => Input?.Invoke(args);

    /// <summary>
    /// The cursor Avalonia last asked for, so the overlay can draw the right
    /// glyph - there is no OS cursor to set when we are not a real window.
    /// </summary>
    public StandardCursorType CurrentCursor { get; private set; } = StandardCursorType.Arrow;

    public event Action<StandardCursorType>? CursorChanged;

    public override void SetCursor(ICursorImpl? cursor)
    {
        // Avalonia hands us a platform cursor object. Win32's implementation
        // does not expose which standard cursor it came from, so recover it
        // from the Cursor wrapper's ToString-able type where possible and fall
        // back to an arrow. Good enough to distinguish the common shapes.
        StandardCursorType resolved = cursor is null
            ? StandardCursorType.Arrow
            : ResolveCursorType(cursor);

        if (resolved == CurrentCursor) return;
        CurrentCursor = resolved;
        CursorChanged?.Invoke(resolved);
    }

    private static StandardCursorType ResolveCursorType(ICursorImpl cursor)
    {
        // Win32's CursorImpl keeps the StandardCursorType it was created from
        // in a private field. Best-effort: a wrong glyph is cosmetic, so never
        // let this throw.
        try
        {
            FieldInfo? field = cursor.GetType().GetField(
                "_cursorType", BindingFlags.Instance | BindingFlags.NonPublic);
            if (field?.GetValue(cursor) is StandardCursorType type) return type;
        }
        catch
        {
            // ignored - fall through to Arrow
        }
        return StandardCursorType.Arrow;
    }

    /// <summary>
    /// The pointer device the base class requires. See
    /// <see cref="RawEventFactory.CreateMouseDevice"/> for why it cannot simply
    /// be constructed.
    /// </summary>
    private static IMouseDevice CreateMouseDevice() => RawEventFactory.CreateMouseDevice();

    /// <summary>
    /// Resizes the logical surface. Avalonia lays out in device-independent
    /// pixels, so the caller divides physical backbuffer pixels by the scaling
    /// it wants; the framebuffer itself is always sized in physical pixels.
    /// </summary>
    public void SetSize(int physicalWidth, int physicalHeight, double scaling)
    {
        RenderScaling = scaling;
        _surface.Resize(physicalWidth, physicalHeight, scaling);
        ClientSize = new Size(physicalWidth / scaling, physicalHeight / scaling);
        Resized?.Invoke(ClientSize, global::Avalonia.Controls.WindowResizeReason.Layout);
    }
}

/// <summary>
/// Exposes pinned CPU buffers to Avalonia's Skia backend as a framebuffer.
///
/// Skia rasterises straight into <see cref="ILockedFramebuffer.Address"/>, so
/// there is no intermediate bitmap: the bytes Avalonia writes are the bytes we
/// hand to Direct3D.
/// </summary>
/// <remarks>
/// <para>
/// Buffers are double-buffered and the lock is held only for pointer
/// bookkeeping - never across Avalonia's render pass. Holding an application
/// lock for the duration of rasterisation deadlocks: Avalonia's render thread
/// synchronises with its UI thread, so if the UI thread then needs the same
/// lock (which it does on resize), the two block on each other.
/// </para>
/// <para>
/// Alternating buffers is safe because Avalonia redraws the whole surface every
/// frame here - <c>FramebufferRenderTarget.Properties.RetainsPreviousFrameContents</c>
/// is false unless the render target implements
/// <c>IFramebufferRenderTargetWithProperties</c>, which this one deliberately
/// does not.
/// </para>
/// <para>
/// The buffers are pinned managed arrays rather than native allocations so that
/// a resize cannot free memory a render still in flight is writing into; the
/// old array simply stays alive until that render lets go of it.
/// </para>
/// </remarks>
internal sealed unsafe class SharedTextureFramebufferSurface : IFramebufferPlatformSurface, IDisposable
{
    private sealed class Frame
    {
        public required byte[] Pixels { get; init; }      // pinned
        public required int Width { get; init; }
        public required int Height { get; init; }
        public required int Generation { get; init; }
        public int RowBytes => Width * 4;
    }

    private readonly object _sync = new();

    private Frame[] _pool = [];
    private Frame? _ready;      // rendered, waiting to be uploaded
    private Frame? _reading;    // currently being uploaded by the session
    private int _generation;
    private double _scaling = 1.0;
    private bool _disposed;

    public void Resize(int width, int height, double scaling)
    {
        lock (_sync)
        {
            _scaling = scaling;
            if (_pool.Length > 0 && _pool[0].Width == width && _pool[0].Height == height) return;

            int generation = ++_generation;
            _pool =
            [
                CreateFrame(width, height, generation),
                CreateFrame(width, height, generation),
            ];

            // Anything produced at the old size is now meaningless.
            _ready = null;
            _reading = null;
        }
    }

    private static Frame CreateFrame(int width, int height, int generation) => new()
    {
        // Pinned so Skia can write to a stable address, and managed so its
        // lifetime is not our problem during a resize.
        Pixels = GC.AllocateArray<byte>(width * height * 4, pinned: true),
        Width = width,
        Height = height,
        Generation = generation,
    };

    public IFramebufferRenderTarget CreateFramebufferRenderTarget() => new RenderTarget(this);

    /// <summary>Picks a buffer to render into, avoiding the one being uploaded.</summary>
    private Frame? AcquireForRender()
    {
        lock (_sync)
        {
            if (_disposed || _pool.Length == 0) return null;
            foreach (Frame frame in _pool)
            {
                if (!ReferenceEquals(frame, _reading)) return frame;
            }
            return null;
        }
    }

    private void PublishRendered(Frame frame)
    {
        lock (_sync)
        {
            // Drop frames from before a resize rather than handing the producer
            // a buffer whose size no longer matches the shared texture.
            if (frame.Generation == _generation) _ready = frame;
        }
    }

    /// <summary>
    /// Takes the most recent completed frame for upload. Returns false when
    /// Avalonia has not produced anything new, which is the common case - a
    /// static UI should not burn bandwidth re-uploading identical pixels.
    /// </summary>
    public bool TryBeginRead(out IntPtr buffer, out int rowBytes, out int width, out int height)
    {
        buffer = IntPtr.Zero;
        rowBytes = width = height = 0;

        Frame frame;
        lock (_sync)
        {
            if (_ready is null) return false;
            frame = _ready;
            _ready = null;
            _reading = frame;
        }

        buffer = (IntPtr)Unsafe.AsPointer(ref frame.Pixels[0]);
        rowBytes = frame.RowBytes;
        width = frame.Width;
        height = frame.Height;
        return true;
    }

    public void EndRead()
    {
        lock (_sync) _reading = null;
    }

    public void Dispose()
    {
        lock (_sync)
        {
            _disposed = true;
            _pool = [];
            _ready = null;
            _reading = null;
        }
    }

    private sealed class RenderTarget : IFramebufferRenderTarget
    {
        private readonly SharedTextureFramebufferSurface _owner;

        public RenderTarget(SharedTextureFramebufferSurface owner) => _owner = owner;

        public ILockedFramebuffer Lock()
        {
            Frame frame = _owner.AcquireForRender()
                ?? throw new InvalidOperationException("Framebuffer surface is not sized yet.");

            return new LockedFramebuffer(_owner, frame);
        }

        public void Dispose() { }
    }

    private sealed class LockedFramebuffer : ILockedFramebuffer
    {
        private readonly SharedTextureFramebufferSurface _owner;
        private readonly Frame _frame;

        public LockedFramebuffer(SharedTextureFramebufferSurface owner, Frame frame)
        {
            _owner = owner;
            _frame = frame;
            Address = (IntPtr)Unsafe.AsPointer(ref frame.Pixels[0]);
            Size = new PixelSize(frame.Width, frame.Height);
            RowBytes = frame.RowBytes;
            Dpi = new Vector(96 * owner._scaling, 96 * owner._scaling);
            // Premultiplied to match the payload's (ONE, INV_SRC_ALPHA) blend,
            // and BGRA so the upload is a straight memcpy into the D3D11
            // B8G8R8A8_UNORM texture with no swizzle.
            Format = PixelFormat.Bgra8888;
        }

        public IntPtr Address { get; }
        public PixelSize Size { get; }
        public int RowBytes { get; }
        public Vector Dpi { get; }
        public PixelFormat Format { get; }

        public void Dispose() => _owner.PublishRendered(_frame);
    }
}
