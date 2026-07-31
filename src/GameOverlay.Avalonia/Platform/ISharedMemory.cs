using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// A named, cross-process shared-memory region mapped read/write into this
/// process, exposed as a raw pointer.
/// </summary>
/// <remarks>
/// This is the one primitive both shared-memory consumers need - the control
/// block (<see cref="OverlaySharedState"/>) and the D3D9 CPU pixel buffer
/// (<see cref="CpuFrameProducer"/>). Windows backs it with a named
/// <c>MemoryMappedFile</c>; Linux/macOS back it with <c>shm_open</c>/<c>mmap</c>.
/// The caller passes a platform-neutral logical name (e.g.
/// <c>AvaloniaOverlay.State.1234</c>); the implementation applies whatever OS
/// prefix its named-object namespace requires, matching the payload's own
/// naming on that platform.
/// </remarks>
internal unsafe interface ISharedMemory : IDisposable
{
    /// <summary>The mapped base address. Valid until <see cref="IDisposable.Dispose"/>.</summary>
    byte* Pointer { get; }

    /// <summary>The mapped region size in bytes.</summary>
    long Length { get; }
}
