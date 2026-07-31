using System.IO.MemoryMappedFiles;

namespace GameOverlay.Avalonia;

/// <summary>
/// Windows <see cref="ISharedMemory"/> over a named <c>MemoryMappedFile</c>.
/// </summary>
/// <remarks>
/// The <c>Local\</c> session prefix lives here (not in the neutral callers) so
/// the OS object name matches the payload's <c>FormatMappingName</c>/
/// <c>FormatFrameMappingName</c> exactly, while callers deal only in logical
/// names.
/// </remarks>
internal sealed unsafe class WindowsSharedMemory : ISharedMemory
{
    private readonly MemoryMappedFile _mmf;
    private readonly MemoryMappedViewAccessor _view;
    private byte* _pointer;

    public byte* Pointer => _pointer;
    public long Length { get; }

    public WindowsSharedMemory(string logicalName, long size)
    {
        // Create-or-open so it does not matter whether the host or the payload
        // maps it first.
        string name = $"Local\\{logicalName}";
        _mmf = MemoryMappedFile.CreateOrOpen(name, size, MemoryMappedFileAccess.ReadWrite);
        _view = _mmf.CreateViewAccessor(0, size, MemoryMappedFileAccess.ReadWrite);

        byte* ptr = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref ptr);
        _pointer = ptr;
        Length = size;
    }

    public void Dispose()
    {
        if (_pointer is not null)
        {
            _view.SafeMemoryMappedViewHandle.ReleasePointer();
            _pointer = null;
        }
        _view.Dispose();
        _mmf.Dispose();
    }
}
