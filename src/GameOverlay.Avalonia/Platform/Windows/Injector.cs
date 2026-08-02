using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// Owns a handle to the target game process and performs the operations that
/// need it: loading the payload, duplicating the shared-texture handle across
/// the process boundary, and unloading cleanly.
/// </summary>
internal sealed partial class GameProcess : IProcessInjector
{
    private const uint PROCESS_CREATE_THREAD = 0x0002;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_DUP_HANDLE = 0x0040;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;

    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint MEM_RELEASE = 0x8000;
    private const uint PAGE_READWRITE = 0x04;

    private const uint DUPLICATE_CLOSE_SOURCE = 0x0001;
    private const uint DUPLICATE_SAME_ACCESS = 0x0002;
    private const uint INFINITE = 0xFFFFFFFF;

    private const uint CREATE_SUSPENDED = 0x00000004;

    private readonly IntPtr _handle;
    private readonly Action<string>? _log;
    private IntPtr _mainThread;
    private bool _is32Bit;
    private bool _disposed;

    public int Pid { get; }

    /// <summary>
    /// True when the target runs under WoW64 (a 32-bit game on 64-bit Windows).
    /// Detected once at open/launch; drives payload selection and the injection
    /// path.
    /// </summary>
    public bool Is32BitTarget => _is32Bit;

    /// <summary>True when the process is waiting to be resumed after injection.</summary>
    public bool IsSuspended => _mainThread != IntPtr.Zero;

    private GameProcess(IntPtr handle, int pid, Action<string>? log, IntPtr mainThread = default)
    {
        _handle = handle;
        Pid = pid;
        _log = log;
        _mainThread = mainThread;
    }

    private void Log(string message) => _log?.Invoke(message);

    /// <summary>
    /// Starts a game with its main thread suspended, so the payload can be
    /// injected before the game executes a single instruction.
    /// </summary>
    /// <remarks>
    /// Required for Vulkan, and better for the D3D backends too. Vulkan will
    /// not let a device import external memory unless
    /// <c>VK_KHR_external_memory_win32</c> was enabled when the device was
    /// created - and a device cannot be re-created. So the overlay has to be in
    /// place to rewrite <c>vkCreateDevice</c>'s extension list before the game
    /// calls it; attaching to an already-running Vulkan game is too late.
    /// </remarks>
    public static GameProcess LaunchSuspended(string exePath, string? arguments = null, Action<string>? log = null)
    {
        exePath = Path.GetFullPath(exePath);
        if (!File.Exists(exePath)) throw new FileNotFoundException("Game not found", exePath);

        var startupInfo = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };

        // CreateProcess may modify the command line buffer, so it cannot be a
        // literal.
        var commandLine = new StringBuilder($"\"{exePath}\" {arguments}".TrimEnd());

        if (!CreateProcess(exePath, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                           CREATE_SUSPENDED, IntPtr.Zero,
                           Path.GetDirectoryName(exePath), ref startupInfo,
                           out PROCESS_INFORMATION info))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"CreateProcess('{exePath}') failed");
        }

        var process = new GameProcess(info.hProcess, info.dwProcessId, log, info.hThread);
        process.DetectBitness();
        process.Log($"[inject] launched {Path.GetFileName(exePath)} suspended as pid {info.dwProcessId}" +
                    (process._is32Bit ? " (32-bit)" : string.Empty));
        return process;
    }

    /// <summary>Lets a suspended game start running. Idempotent.</summary>
    public void ResumeMainThread()
    {
        if (_mainThread == IntPtr.Zero) return;

        if (ResumeThread(_mainThread) == unchecked((uint)-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ResumeThread failed");

        CloseHandle(_mainThread);
        _mainThread = IntPtr.Zero;
        Log("[inject] game resumed");
    }

    public static GameProcess Open(int pid, Action<string>? log = null)
    {
        const uint access = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ
                          | PROCESS_VM_WRITE | PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION;

        IntPtr handle = OpenProcess(access, false, pid);
        if (handle == IntPtr.Zero)
        {
            int err = Marshal.GetLastWin32Error();
            string hint = err == 5
                ? " (access denied - if the game runs elevated, the host must too)"
                : string.Empty;
            throw new Win32Exception(err, $"OpenProcess({pid}) failed{hint}");
        }

        var process = new GameProcess(handle, pid, log);
        process.DetectBitness();
        return process;
    }

    /// <summary>
    /// Records whether the target is 32-bit (WoW64). Bitness decides which
    /// payload is injected and how <c>LoadLibraryW</c>/<c>OverlayDetach</c> are
    /// addressed in the target; picking the wrong one is a confusing remote-thread
    /// crash rather than a clear error, so it is settled once up front.
    /// </summary>
    private void DetectBitness()
    {
        // IsWow64Process reports true only for a 32-bit process on 64-bit Windows,
        // which is exactly the case that needs the x86 payload. A failed call
        // (e.g. on 32-bit Windows, which the x64 host does not run on) leaves the
        // default of "same bitness as the host".
        _is32Bit = IsWow64Process(_handle, out bool isWow64) && isWow64;
    }

    // --- payload loading ---------------------------------------------------

    public bool IsPayloadLoaded(string dllPath) => FindRemoteModule(dllPath) != IntPtr.Zero;

    public void InjectPayload(string dllPath)
    {
        dllPath = Path.GetFullPath(dllPath);
        if (!File.Exists(dllPath))
            throw new FileNotFoundException("Overlay payload not found", dllPath);

        if (IsPayloadLoaded(dllPath))
        {
            Log($"[inject] payload already present in pid {Pid}");
            return;
        }

        byte[] pathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
        IntPtr remoteBuffer = VirtualAllocEx(_handle, IntPtr.Zero, (nuint)pathBytes.Length,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remoteBuffer == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "VirtualAllocEx failed");

        try
        {
            if (!WriteProcessMemory(_handle, remoteBuffer, pathBytes, (nuint)pathBytes.Length, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory failed");

            IntPtr loadLibrary = ResolveRemoteLoadLibraryW();

            IntPtr thread = CreateRemoteThread(_handle, IntPtr.Zero, 0, loadLibrary,
                                               remoteBuffer, 0, out _);
            if (thread == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateRemoteThread failed");

            try
            {
                WaitForSingleObject(thread, INFINITE);
                // The thread's exit code is a truncated HMODULE on x64, so it
                // cannot be trusted; confirm by looking the module up instead.
                if (!IsPayloadLoaded(dllPath))
                {
                    throw new InvalidOperationException(
                        "LoadLibraryW returned but the payload is not in the module list. " +
                        "A missing dependency or a security product blocking the load are " +
                        "the usual causes.");
                }
            }
            finally
            {
                CloseHandle(thread);
            }
        }
        finally
        {
            VirtualFreeEx(_handle, remoteBuffer, 0, MEM_RELEASE);
        }

        Log($"[inject] payload loaded into pid {Pid}");
    }

    /// <summary>
    /// Resolves the address of <c>kernel32!LoadLibraryW</c> as the target process
    /// sees it, to use as a <see cref="CreateRemoteThread"/> start routine.
    /// </summary>
    private IntPtr ResolveRemoteLoadLibraryW()
    {
        if (!_is32Bit)
        {
            // Same bitness: kernel32 loads at the same base in every same-arch
            // process in a session, so our own LoadLibraryW address is valid in
            // the target too. This is the original, battle-tested path.
            IntPtr kernel32 = GetModuleHandle("kernel32.dll");
            IntPtr loadLibrary = GetProcAddress(kernel32, "LoadLibraryW");
            if (loadLibrary == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "GetProcAddress(LoadLibraryW) failed");
            return loadLibrary;
        }

        // Cross-bitness (x64 host -> x86 target): our 64-bit LoadLibraryW address
        // means nothing in the target's 32-bit kernel32, which sits at a different
        // base. Rebuild the address from the target's 32-bit kernel32 base plus
        // LoadLibraryW's RVA read from the on-disk SysWOW64 copy. An export RVA is
        // not rebased at load time, so base + RVA is exact.
        IntPtr remoteKernel32 = FindRemoteModuleByLeafName("kernel32.dll");
        if (remoteKernel32 == IntPtr.Zero)
            throw new InvalidOperationException(
                $"Could not locate the 32-bit kernel32.dll in process {Pid}. The WoW64 " +
                "environment may not be initialised yet.");

        // SystemX86 resolves to SysWOW64 on 64-bit Windows: the 32-bit system DLLs.
        string kernel32Path = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "kernel32.dll");
        uint rva = PeExports.GetExportRva(kernel32Path, "LoadLibraryW");
        return remoteKernel32 + (nint)rva;
    }

    /// <summary>
    /// Calls the payload's exported OverlayDetach on a remote thread, which
    /// unhooks and then frees itself.
    /// </summary>
    public void DetachPayload(string dllPath)
    {
        IntPtr remoteBase = FindRemoteModule(dllPath);
        if (remoteBase == IntPtr.Zero) return;

        // Rebase OverlayDetach's RVA (read from the payload file) onto the remote
        // module. Reading the RVA from disk - rather than LoadLibrary-ing our own
        // copy to ask GetProcAddress - is what makes this bitness-agnostic: a
        // 64-bit host cannot load a 32-bit payload, but it can still parse its
        // export table. An export RVA is not rebased at load, so base + RVA is the
        // export's address in the game.
        uint rva;
        try
        {
            rva = PeExports.GetExportRva(Path.GetFullPath(dllPath), "OverlayDetach");
        }
        catch (Exception ex)
        {
            Log($"[inject] cannot resolve OverlayDetach in {dllPath}: {ex.Message}");
            return;
        }

        IntPtr remoteExport = remoteBase + (nint)rva;
        IntPtr thread = CreateRemoteThread(_handle, IntPtr.Zero, 0, remoteExport,
                                           IntPtr.Zero, 0, out _);
        if (thread == IntPtr.Zero) return;

        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        Log($"[inject] payload detached from pid {Pid}");
    }

    private IntPtr FindRemoteModule(string dllPath)
    {
        string target = Path.GetFullPath(dllPath);

        // A game can have a lot of modules; 1024 is comfortably above any real
        // process and avoids a second round trip.
        IntPtr[] modules = new IntPtr[1024];
        int byteSize = modules.Length * IntPtr.Size;
        if (!EnumProcessModulesEx(_handle, modules, (uint)byteSize, out uint needed, 0x03))
            return IntPtr.Zero;

        int count = (int)Math.Min((uint)modules.Length, needed / (uint)IntPtr.Size);
        var buffer = new StringBuilder(1024);
        for (int i = 0; i < count; i++)
        {
            buffer.Clear();
            if (GetModuleFileNameEx(_handle, modules[i], buffer, buffer.Capacity) == 0) continue;
            if (string.Equals(buffer.ToString(), target, StringComparison.OrdinalIgnoreCase))
                return modules[i];
        }
        return IntPtr.Zero;
    }

    /// <summary>
    /// Finds a loaded module in the target by its file name alone (e.g.
    /// "kernel32.dll"), returning its base address. Used to locate the target's
    /// 32-bit kernel32 for cross-bitness injection, where its full path
    /// (SysWOW64) is not known ahead of time.
    /// </summary>
    private IntPtr FindRemoteModuleByLeafName(string leafName)
    {
        IntPtr[] modules = new IntPtr[1024];
        int byteSize = modules.Length * IntPtr.Size;
        // 0x03 = LIST_MODULES_ALL, which includes a WoW64 target's 32-bit modules.
        if (!EnumProcessModulesEx(_handle, modules, (uint)byteSize, out uint needed, 0x03))
            return IntPtr.Zero;

        int count = (int)Math.Min((uint)modules.Length, needed / (uint)IntPtr.Size);
        var buffer = new StringBuilder(1024);
        for (int i = 0; i < count; i++)
        {
            buffer.Clear();
            if (GetModuleFileNameEx(_handle, modules[i], buffer, buffer.Capacity) == 0) continue;
            if (string.Equals(Path.GetFileName(buffer.ToString()), leafName, StringComparison.OrdinalIgnoreCase))
                return modules[i];
        }
        return IntPtr.Zero;
    }

    // --- handle transport --------------------------------------------------

    /// <summary>
    /// Copies a handle from this process into the game, returning the handle
    /// value as the game sees it. This is how the shared texture's NT handle
    /// crosses the process boundary; the raw value would be meaningless there.
    /// </summary>
    public IntPtr DuplicateHandleInto(IntPtr localHandle)
    {
        IntPtr self = GetCurrentProcess();
        if (!DuplicateHandle(self, localHandle, _handle, out IntPtr remoteHandle,
                             0, false, DUPLICATE_SAME_ACCESS))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "DuplicateHandle into the game process failed");
        }
        return remoteHandle;
    }

    /// <summary>
    /// Copies a handle the payload created in the game into this process.
    /// </summary>
    /// <remarks>
    /// The reverse of <see cref="DuplicateHandleInto"/>, needed because on
    /// D3D12 the payload owns one half of the fence pair and its handle value
    /// is only meaningful inside the game.
    /// </remarks>
    public IntPtr DuplicateHandleFrom(IntPtr handleInGameProcess)
    {
        if (!DuplicateHandle(_handle, handleInGameProcess, GetCurrentProcess(),
                             out IntPtr localHandle, 0, false, DUPLICATE_SAME_ACCESS))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "DuplicateHandle out of the game process failed");
        }
        return localHandle;
    }

    /// <summary>
    /// Closes a handle we previously duplicated into the game. Without this,
    /// every swapchain resize would strand another shared-texture handle - and
    /// its GPU memory - in the game process for as long as it runs.
    /// </summary>
    public void CloseRemoteHandle(IntPtr remoteHandle)
    {
        if (remoteHandle == IntPtr.Zero) return;

        // DUPLICATE_CLOSE_SOURCE closes the handle in the source process; the
        // duplicate it produces here is immediately discarded.
        if (DuplicateHandle(_handle, remoteHandle, GetCurrentProcess(), out IntPtr scratch,
                            0, false, DUPLICATE_CLOSE_SOURCE))
        {
            CloseHandle(scratch);
        }
    }

    public bool IsAlive
    {
        get
        {
            if (_disposed) return false;
            return GetExitCodeProcess(_handle, out uint code) && code == 259; // STILL_ACTIVE
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        // Never leave a process suspended forever because we failed part way
        // through - that would strand it invisibly.
        if (_mainThread != IntPtr.Zero)
        {
            ResumeThread(_mainThread);
            CloseHandle(_mainThread);
            _mainThread = IntPtr.Zero;
        }

        CloseHandle(_handle);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct STARTUPINFO
    {
        public int cb;
        public IntPtr lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcess(string? applicationName, StringBuilder commandLine,
                                             IntPtr processAttributes, IntPtr threadAttributes,
                                             bool inheritHandles, uint creationFlags,
                                             IntPtr environment, string? currentDirectory,
                                             ref STARTUPINFO startupInfo,
                                             out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);

    // --- P/Invoke ----------------------------------------------------------

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial IntPtr OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, int pid);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(IntPtr handle);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial IntPtr VirtualAllocEx(IntPtr process, IntPtr address, nuint size,
                                                 uint allocationType, uint protect);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool VirtualFreeEx(IntPtr process, IntPtr address, nuint size, uint freeType);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool WriteProcessMemory(IntPtr process, IntPtr address,
                                                   [In] byte[] buffer, nuint size, out nuint written);

    // LibraryImport does not append the A/W suffix that DllImport's CharSet
    // would, so the wide entry point has to be named explicitly.
    [LibraryImport("kernel32.dll", SetLastError = true, StringMarshalling = StringMarshalling.Utf16,
                   EntryPoint = "GetModuleHandleW")]
    private static partial IntPtr GetModuleHandle(string name);

    // GetProcAddress takes LPCSTR; export names are ASCII, for which UTF-8 is
    // byte-identical.
    [LibraryImport("kernel32.dll", SetLastError = true, StringMarshalling = StringMarshalling.Utf8)]
    private static partial IntPtr GetProcAddress(IntPtr module, string name);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes, nuint stackSize,
                                                     IntPtr startAddress, IntPtr parameter,
                                                     uint flags, out uint threadId);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial IntPtr GetCurrentProcess();

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool DuplicateHandle(IntPtr sourceProcess, IntPtr sourceHandle,
                                                IntPtr targetProcess, out IntPtr targetHandle,
                                                uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit,
                                                uint options);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool IsWow64Process(IntPtr process, [MarshalAs(UnmanagedType.Bool)] out bool wow64);

    [LibraryImport("psapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool EnumProcessModulesEx(IntPtr process, [Out] IntPtr[] modules,
                                                     uint size, out uint needed, uint filter);

    [DllImport("psapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetModuleFileNameEx(IntPtr process, IntPtr module,
                                                   StringBuilder name, int size);
}
