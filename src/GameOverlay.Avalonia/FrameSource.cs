using System;
using System.Runtime.InteropServices;

namespace GameOverlay.Avalonia;

/// <summary>
/// Produces frames of premultiplied BGRA pixels for the producer to upload.
/// Everything above the transport is expressed through this, so the test
/// pattern and the real Avalonia renderer are interchangeable.
/// </summary>
/// <remarks>
/// The begin/end shape exists because Avalonia rasterises on its own render
/// thread. The session must be able to hold the pixel buffer still for the
/// duration of the upload, and a bare <c>Buffer</c> property could not express
/// that.
/// </remarks>
internal interface IFrameSource : IDisposable
{
    void Resize(int width, int height);

    /// <summary>
    /// Returns false when there is no new content to publish. On true, the
    /// buffer stays valid and unmodified until <see cref="EndFrame"/>.
    /// </summary>
    /// <remarks>
    /// The dimensions are reported per frame rather than assumed from the last
    /// <see cref="Resize"/>, because a source may apply a resize
    /// asynchronously. Publishing a buffer whose size does not match the
    /// destination texture reads off the end of it.
    /// </remarks>
    bool TryBeginFrame(out IntPtr buffer, out int rowBytes, out int width, out int height);

    void EndFrame();
}

/// <summary>
/// A dependency-free frame source used to prove out injection, handle
/// duplication, keyed-mutex sync and compositing before Avalonia enters the
/// picture. If this does not appear over the game, the problem is in the
/// transport, not in the UI framework.
/// </summary>
internal sealed unsafe class TestPatternFrameSource : IFrameSource
{
    private byte* _buffer;
    private int _width;
    private int _height;
    private int _frame;

    public void Resize(int width, int height)
    {
        if (width == _width && height == _height && _buffer != null) return;

        if (_buffer != null) NativeMemory.Free(_buffer);
        _width = width;
        _height = height;
        _buffer = (byte*)NativeMemory.Alloc((nuint)(width * height * 4));
        NativeMemory.Clear(_buffer, (nuint)(width * height * 4));
    }

    public bool TryBeginFrame(out IntPtr buffer, out int rowBytes, out int width, out int height)
    {
        buffer = IntPtr.Zero;
        rowBytes = 0;
        width = _width;
        height = _height;
        if (_buffer == null) return false;

        _frame++;
        Render();

        buffer = (IntPtr)_buffer;
        rowBytes = _width * 4;
        return true;
    }

    public void EndFrame() { }

    private void Render()
    {
        NativeMemory.Clear(_buffer, (nuint)(_width * _height * 4));

        // A translucent panel with a moving highlight bar. Motion matters: a
        // static image cannot distinguish "compositing works" from "one frame
        // got through and then the pipeline stalled".
        int panelW = Math.Min(560, _width - 80);
        int panelH = Math.Min(260, _height - 80);
        int panelX = 40;
        int panelY = 40;
        int barOffset = (int)((_frame * 3) % Math.Max(1, panelW - 60));

        for (int y = 0; y < panelH; y++)
        {
            byte* row = _buffer + (long)(panelY + y) * (_width * 4) + (long)panelX * 4;
            for (int x = 0; x < panelW; x++)
            {
                bool border = x < 3 || y < 3 || x >= panelW - 3 || y >= panelH - 3;
                bool bar = x >= barOffset && x < barOffset + 60;

                // Straight alpha first, then premultiply on store - the payload
                // blends with (ONE, INV_SRC_ALPHA).
                byte a = border ? (byte)255 : (byte)170;
                byte r = border ? (byte)0 : (bar ? (byte)255 : (byte)20);
                byte g = border ? (byte)200 : (bar ? (byte)140 : (byte)25);
                byte b = border ? (byte)255 : (bar ? (byte)0 : (byte)35);

                row[x * 4 + 0] = (byte)(b * a / 255);
                row[x * 4 + 1] = (byte)(g * a / 255);
                row[x * 4 + 2] = (byte)(r * a / 255);
                row[x * 4 + 3] = a;
            }
        }
    }

    public void Dispose()
    {
        if (_buffer != null)
        {
            NativeMemory.Free(_buffer);
            _buffer = null;
        }
    }
}
