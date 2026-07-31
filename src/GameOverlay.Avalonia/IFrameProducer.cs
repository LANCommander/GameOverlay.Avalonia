using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// Publishes overlay frames to the injected payload. There are two
/// implementations, chosen by the game's graphics API:
///
///   * <see cref="SharedTextureProducer"/> - a shared GPU texture (keyed mutex
///     or fence), used by D3D11/D3D12/Vulkan/OpenGL/D3D10.
///   * <see cref="CpuFrameProducer"/> - a CPU shared-memory copy, used by D3D9,
///     which cannot open any of the GPU-shared textures.
///
/// The session drives both the same way, so it depends only on this shape.
/// </summary>
internal interface IFrameProducer : IDisposable
{
    /// <summary>Width of the current overlay surface, in pixels.</summary>
    int Width { get; }

    /// <summary>Height of the current overlay surface, in pixels.</summary>
    int Height { get; }

    /// <summary>
    /// (Re)creates the overlay surface at the requested size and republishes it
    /// to the payload. A cheap no-op when the size is unchanged.
    /// </summary>
    void EnsureSize(int width, int height);

    /// <summary>
    /// Publishes one frame of premultiplied BGRA pixels. Returns false when the
    /// payload has not yet consumed the previous frame (GPU path) - the caller
    /// should simply try again later. The CPU path never refuses.
    /// </summary>
    bool TryPublishFrame(IntPtr source, int sourceRowBytes);
}
