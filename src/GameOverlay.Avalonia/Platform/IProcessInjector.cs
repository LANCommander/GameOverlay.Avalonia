using System;

namespace GameOverlay.Avalonia;

/// <summary>
/// The target game process together with the OS operations the overlay performs
/// on it: loading and unloading the injected payload, and moving shared-resource
/// handles across the process boundary.
/// </summary>
/// <remarks>
/// There is one implementation per platform. Windows injects an in-process DLL
/// via <c>CreateRemoteThread</c>/<c>LoadLibraryW</c> and moves handles with
/// <c>DuplicateHandle</c> (see the Win32 <c>GameProcess</c>); Linux will preload
/// a shared object at launch and pass file descriptors over a Unix socket. The
/// consumers - <see cref="OverlaySession"/> and the frame producers - depend on
/// this abstraction rather than any one platform's mechanism.
/// </remarks>
internal interface IProcessInjector : IDisposable
{
    /// <summary>The target game's process id.</summary>
    int Pid { get; }

    /// <summary>True while the target process is still running.</summary>
    bool IsAlive { get; }

    /// <summary>True when the process is waiting to be resumed after injection.</summary>
    bool IsSuspended { get; }

    /// <summary>Lets a suspended game start running. Idempotent.</summary>
    void ResumeMainThread();

    /// <summary>Loads the overlay payload into the target process.</summary>
    void InjectPayload(string payloadPath);

    /// <summary>Unloads the overlay payload from the target process cleanly.</summary>
    void DetachPayload(string payloadPath);

    /// <summary>
    /// Copies a handle from this process into the target, returning the handle
    /// value as the target sees it. This is how a shared resource's handle
    /// crosses the process boundary.
    /// </summary>
    IntPtr DuplicateHandleInto(IntPtr localHandle);

    /// <summary>Copies a handle the payload created in the target into this process.</summary>
    IntPtr DuplicateHandleFrom(IntPtr handleInTargetProcess);

    /// <summary>Closes a handle previously duplicated into the target process.</summary>
    void CloseRemoteHandle(IntPtr remoteHandle);
}
