using System;
using System.IO;
using System.Runtime.InteropServices;

namespace GameOverlay.Avalonia;

/// <summary>
/// Linux <see cref="ISharedMemory"/> over a POSIX named shared-memory object
/// (<c>shm_open</c> + <c>ftruncate</c> + <c>mmap</c>) - the direct analogue of
/// the Windows named <c>MemoryMappedFile</c>.
/// </summary>
/// <remarks>
/// The single leading-slash name (<c>/AvaloniaOverlay.State.1234</c>) is the
/// POSIX shm convention and matches the payload's own naming in
/// <c>platform_linux.cpp</c> / the GLX renderer, so host and payload converge on
/// one object.
/// </remarks>
internal sealed unsafe partial class LinuxSharedMemory : ISharedMemory
{
    private const int O_RDWR = 0x0002;
    private const int O_CREAT = 0x0040;   // Linux value (octal 0100)
    private const int PROT_READ = 0x1;
    private const int PROT_WRITE = 0x2;
    private const int MAP_SHARED = 0x01;
    private static readonly IntPtr MAP_FAILED = new(-1);

    private readonly string _name;
    private readonly int _fd;
    private byte* _pointer;

    public byte* Pointer => _pointer;
    public long Length { get; }

    public LinuxSharedMemory(string logicalName, long size)
    {
        _name = "/" + logicalName;

        _fd = shm_open(_name, O_CREAT | O_RDWR, 0x180 /* 0600 */);
        if (_fd < 0)
            throw new IOException($"shm_open('{_name}') failed (errno {Marshal.GetLastPInvokeError()})");

        if (ftruncate(_fd, size) != 0)
        {
            int err = Marshal.GetLastPInvokeError();
            close(_fd);
            throw new IOException($"ftruncate('{_name}', {size}) failed (errno {err})");
        }

        IntPtr addr = mmap(IntPtr.Zero, (nuint)size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
        if (addr == MAP_FAILED)
        {
            int err = Marshal.GetLastPInvokeError();
            close(_fd);
            throw new IOException($"mmap('{_name}') failed (errno {err})");
        }

        _pointer = (byte*)addr;
        Length = size;
    }

    public void Dispose()
    {
        if (_pointer is not null)
        {
            munmap((IntPtr)_pointer, (nuint)Length);
            _pointer = null;
        }
        if (_fd >= 0)
            close(_fd);

        // Best-effort: drop the name so the object does not outlive the session.
        // Removing the name leaves any still-open mapping (e.g. the payload's)
        // valid until it unmaps, so this cannot pull memory out from under the
        // game.
        shm_unlink(_name);
    }

    [LibraryImport("libc", SetLastError = true, StringMarshalling = StringMarshalling.Utf8)]
    private static partial int shm_open(string name, int oflag, uint mode);

    [LibraryImport("libc", SetLastError = true, StringMarshalling = StringMarshalling.Utf8)]
    private static partial int shm_unlink(string name);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int ftruncate(int fd, long length);

    [LibraryImport("libc", SetLastError = true)]
    private static partial IntPtr mmap(IntPtr addr, nuint length, int prot, int flags, int fd, long offset);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int munmap(IntPtr addr, nuint length);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int close(int fd);
}
