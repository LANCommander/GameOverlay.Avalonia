using System;
using System.Diagnostics;
using System.IO;

namespace GameOverlay.Avalonia;

/// <summary>
/// Linux <see cref="IProcessInjector"/>: launches the game with the payload
/// preloaded via <c>LD_PRELOAD</c>.
/// </summary>
/// <remarks>
/// There is no remote-thread injection and nothing to suspend: the dynamic
/// loader binds the payload's exported <c>glXSwapBuffers</c> ahead of libGL's
/// before the game runs a line, so the overlay is in place from the first frame.
/// Attaching to an already-running process (which would need <c>ptrace</c>) is a
/// later milestone; only launch is supported.
/// </remarks>
internal sealed class LinuxProcessInjector : IProcessInjector
{
    private readonly Process _process;
    private bool _disposed;

    public int Pid => _process.Id;
    public bool IsSuspended => false;   // LD_PRELOAD games start running immediately
    public bool Is32BitTarget => false; // the Linux backend targets x86-64 only

    public bool IsAlive
    {
        get { try { return !_disposed && !_process.HasExited; } catch { return false; } }
    }

    private LinuxProcessInjector(Process process) => _process = process;

    public static LinuxProcessInjector Launch(string exePath, string? arguments, Action<string>? log)
    {
        exePath = Path.GetFullPath(exePath);
        if (!File.Exists(exePath)) throw new FileNotFoundException("Game not found", exePath);

        string payload = ResolvePayloadSo();

        var psi = new ProcessStartInfo
        {
            FileName = exePath,
            Arguments = arguments ?? string.Empty,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(exePath) ?? Environment.CurrentDirectory,
        };

        // Prepend rather than replace, so an existing LD_PRELOAD is preserved.
        string existing = psi.Environment.TryGetValue("LD_PRELOAD", out string? v) ? v ?? string.Empty : string.Empty;
        psi.Environment["LD_PRELOAD"] = string.IsNullOrEmpty(existing) ? payload : $"{payload}:{existing}";

        // Enable the Vulkan overlay layer too (it lives beside the .so with its
        // JSON manifest). A GL game never creates a VkInstance so the layer is
        // inert for it; a Vulkan game never calls glXSwapBuffers so LD_PRELOAD is
        // inert for it. Setting both means one launch path covers either API.
        string? layerDir = Path.GetDirectoryName(payload);
        if (!string.IsNullOrEmpty(layerDir))
        {
            // VK_ADD_LAYER_PATH adds to the loader's default search (rather than
            // replacing it like VK_LAYER_PATH), so system layers still resolve.
            string addPath = psi.Environment.TryGetValue("VK_ADD_LAYER_PATH", out string? lp) ? lp ?? "" : "";
            psi.Environment["VK_ADD_LAYER_PATH"] = string.IsNullOrEmpty(addPath) ? layerDir : $"{layerDir}:{addPath}";
            string layers = psi.Environment.TryGetValue("VK_INSTANCE_LAYERS", out string? el) ? el ?? "" : "";
            const string ours = "VK_LAYER_GAMEOVERLAY_present";
            psi.Environment["VK_INSTANCE_LAYERS"] = string.IsNullOrEmpty(layers) ? ours : $"{layers}:{ours}";
        }

        var proc = Process.Start(psi)
                   ?? throw new InvalidOperationException($"Failed to start '{exePath}'");

        log?.Invoke($"[inject] launched {Path.GetFileName(exePath)} pid {proc.Id} (LD_PRELOAD={payload}; VK layer enabled)");
        return new LinuxProcessInjector(proc);
    }

    private static string ResolvePayloadSo()
    {
        string baseDir = AppContext.BaseDirectory;
        string[] candidates =
        {
            Path.Combine(baseDir, "runtimes", "linux-x64", "native", "GameOverlay.Native.so"),
            Path.Combine(baseDir, "GameOverlay.Native.so"),
        };
        foreach (string c in candidates)
            if (File.Exists(c)) return c;

        // Dev convenience: walk up to a CMake build-linux/bin tree.
        for (var dir = new DirectoryInfo(baseDir); dir is not null; dir = dir.Parent)
        {
            string build = Path.Combine(dir.FullName, "build-linux", "bin", "GameOverlay.Native.so");
            if (File.Exists(build)) return build;
        }

        throw new FileNotFoundException(
            "GameOverlay.Native.so was not found next to the application, under " +
            "runtimes/linux-x64/native, or in a build-linux/bin tree.");
    }

    // Nothing to resume: the game is already running with the payload preloaded.
    public void ResumeMainThread() { }

    // The payload is loaded by the dynamic loader at launch, so there is nothing
    // to inject or unload after the fact; process exit reclaims it.
    public void InjectPayload(string payloadPath) { }
    public void DetachPayload(string payloadPath) { }

    // GPU handle transport is a Windows-only concern; the Linux CPU frame path
    // shares memory by name, not by duplicated handle.
    public IntPtr DuplicateHandleInto(IntPtr localHandle)
        => throw new PlatformNotSupportedException("Handle transport is not used by the Linux CPU frame path.");

    public IntPtr DuplicateHandleFrom(IntPtr handleInTargetProcess)
        => throw new PlatformNotSupportedException("Handle transport is not used by the Linux CPU frame path.");

    public void CloseRemoteHandle(IntPtr remoteHandle) { }

    public void Dispose()
    {
        _disposed = true;
        _process.Dispose();
    }
}
