using System;
using System.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Publishes overlay frames to a D3D9 payload through a CPU shared-memory copy.
///
/// D3D9 cannot open any of the GPU-shared textures the other backends use
/// (it predates DXGI, NT-handle sharing and keyed mutexes), and - as the D3D10
/// work found - sharing a host-created texture into a legacy/layered device is
/// unreliable in practice. So this producer sidesteps GPU sharing entirely: it
/// copies Avalonia's premultiplied BGRA output into a second named mapping, and
/// the payload uploads that into its own dynamic texture each present.
///
/// The cost is one CPU copy per published frame on the host thread; the payload
/// pays a second copy on the game's thread. That is more than the zero-copy GPU
/// path, but it is bounded, predictable, and the only thing that works for a
/// legacy D3D9 (non-Ex) game.
/// </summary>
internal sealed unsafe class CpuFrameProducer : IFrameProducer
{
    private readonly OverlaySharedState _state;
    private readonly Action<string>? _log;

    private ISharedMemory? _shm;
    private byte* _pixels;
    private long _bufferBytes;

    private uint _generation;
    private uint _seq;

    public int Width { get; private set; }
    public int Height { get; private set; }

    public CpuFrameProducer(OverlaySharedState state, Action<string>? log = null)
    {
        _state = state;
        _log = log;
    }

    private void Log(string message) => _log?.Invoke(message);

    /// <summary>
    /// (Re)creates the pixel mapping at the requested size under a fresh
    /// generation and publishes it. The generation is part of the mapping name,
    /// so the payload reopens cleanly on a resize instead of racing a section
    /// whose size cannot change once named.
    /// </summary>
    public void EnsureSize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        if (_shm is not null && width == Width && height == Height) return;

        Release();

        Width = width;
        Height = height;
        _bufferBytes = (long)width * height * 4;

        // A brand-new generation and therefore a brand-new mapping name. While
        // writing must be paused across the swap, the seqlock starts even (0),
        // which reads as "no frame yet" until the first publish.
        _generation++;
        _seq = 0;

        // Round the mapping up to a page so the Linux Vulkan payload can import
        // it directly as image memory (VK_EXT_external_memory_host needs a
        // page-aligned pointer over a page-multiple size). Only the mapping grows;
        // the pixel copy below still touches exactly width*height*4 bytes, so this
        // is harmless to every other consumer.
        long mappingBytes = (_bufferBytes + 4095) / 4096 * 4096;
        string name = _state.FrameMappingName(_generation);
        _shm = PlatformServices.CreateOrOpenSharedMemory(name, mappingBytes);
        _pixels = _shm.Pointer;

        // Publish size and reset the seqlock before the generation, so the
        // payload never derives the new mapping name and then reads a stale
        // sequence value from the previous surface.
        _state.TexWidth = (uint)width;
        _state.TexHeight = (uint)height;
        _state.CpuFrameSeq = 0;
        Thread.MemoryBarrier();
        _state.CpuFrameGeneration = _generation;

        Log($"[producer] cpu frame mapping {width}x{height} gen {_generation} ('{name}')");
    }

    /// <summary>
    /// Copies one frame into the mapping under the seqlock. Never refuses: the
    /// payload polls independently and simply keeps its previous frame if it
    /// catches a write in progress, so there is no back-pressure to report.
    /// </summary>
    public bool TryPublishFrame(IntPtr source, int sourceRowBytes)
    {
        if (_pixels is null || _bufferBytes == 0) return false;

        int dstRowBytes = Width * 4;
        int rowBytes = Math.Min(sourceRowBytes, dstRowBytes);

        // Seqlock write: make the count odd ("writing"), copy, then bump it to
        // the next even value. The barriers keep the payload from seeing an even
        // count before the pixels behind it have landed.
        uint start = _seq;
        _state.CpuFrameSeq = start | 1u;
        Thread.MemoryBarrier();

        byte* dst = _pixels;
        byte* src = (byte*)source;
        for (int y = 0; y < Height; y++)
        {
            Buffer.MemoryCopy(src + (long)y * sourceRowBytes,
                              dst + (long)y * dstRowBytes,
                              dstRowBytes, rowBytes);
        }

        Thread.MemoryBarrier();
        _seq = (start | 1u) + 1u;    // even again, one whole frame newer
        _state.CpuFrameSeq = _seq;

        _state.AdvanceFrame();
        return true;
    }

    private void Release()
    {
        _pixels = null;
        _shm?.Dispose();
        _shm = null;
        _bufferBytes = 0;
    }

    public void Dispose() => Release();
}
