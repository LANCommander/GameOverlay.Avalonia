using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace GameOverlay.Avalonia;

/// <summary>
/// C# mirror of the <c>overlay::SharedState</c> struct in shared_state.h.
/// Field order and types must match exactly; the static constructor asserts
/// the size so a divergence fails immediately instead of corrupting the game.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct SharedState
{
    // --- written by the DLL ---
    public uint AbiVersion;
    public uint GamePid;
    public ulong AdapterLuid;
    public ulong GameHwnd;
    public uint GameWidth;
    public uint GameHeight;
    public uint SwapchainGeneration;
    public uint PresentCount;
    public uint DllAttached;
    public uint BackbufferIsSrgb;

    // --- written by the host ---
    public ulong SharedHandle;
    public uint TexWidth;
    public uint TexHeight;
    public uint FrameIndex;
    public uint Visible;
    public uint InputCapture;
    public uint HostPid;
    public uint HostHeartbeat;

    // --- diagnostics, written by the DLL ---
    public uint DrawCount;
    public uint MutexTimeoutCount;
    public uint InputPushCount;
    public uint InputSeenCount;
    public uint GraphicsApi;

    // --- D3D12 synchronisation (see shared_state.h) ---
    public ulong ProduceFenceHandle;
    public ulong ConsumeFenceHandle;
    public ulong ProduceFenceValue;

    // --- D3D9 CPU frame transport (see shared_state.h) ---
    public uint CpuFrameGeneration;
    public uint CpuFrameSeq;

    // reserved[2]
    private uint _r2, _r3;
}

/// <summary>The graphics API a game renders with, reported by the payload.</summary>
public enum GameGraphicsApi : uint
{
    Unknown = 0,
    // D3D8 predates DXGI and, like D3D9, has no GPU-shared texture path; it
    // reuses the same CPU shared-memory frame transport.
    D3D8 = 8,
    // D3D9 has no GPU-shared texture path; it takes the CPU shared-memory frame
    // transport instead. See <see cref="CpuFrameProducer"/> and shared_state.h.
    D3D9 = 9,
    // D3D10 predates NT-handle sharing and takes a legacy keyed-mutex texture;
    // see <see cref="SharedTextureProducer"/> and shared_state.h.
    D3D10 = 10,
    D3D11 = 11,
    D3D12 = 12,
    Vulkan = 13,
    OpenGL = 14,
}

/// <summary>What the payload observed. Mirrors <c>overlay::InputEventType</c>.</summary>
internal enum InputEventType : uint
{
    None = 0,
    MouseMove,          // X,Y = client pixels
    MouseMoveDelta,     // X,Y = raw relative delta
    MouseDown,          // Data = MouseButton
    MouseUp,
    MouseWheel,         // Data = signed wheel delta, reinterpreted as int
    MouseHWheel,
    KeyDown,            // Data = virtual key, Y = lParam key data
    KeyUp,
    Char,               // Data = UTF-16 code unit
}

internal enum SharedMouseButton : uint
{
    Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4,
}

/// <summary>Modifier bitmask owned by the IPC contract, mirroring <c>kMod*</c>.</summary>
[Flags]
internal enum SharedModifiers : uint
{
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    LeftButton = 1 << 3,
    RightButton = 1 << 4,
    MiddleButton = 1 << 5,
}

/// <summary>Mirrors <c>overlay::InputEvent</c>.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct SharedInputEvent
{
    public InputEventType Type;
    public int X;
    public int Y;
    public uint Data;
    public SharedModifiers Modifiers;
}

/// <summary>Mirrors <c>overlay::InputRing</c>.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct InputRingHeader
{
    public uint WriteIndex;
    public uint ReadIndex;
    public uint Dropped;
    public uint Pad;
    // Followed by SharedInputEvent[OverlaySharedState.InputRingCapacity].
}

/// <summary>
/// Typed accessor over the memory-mapped control block shared with the
/// injected payload. Reads and writes go straight to the mapping - there is no
/// local copy, because both processes are writing to it concurrently.
/// </summary>
internal sealed unsafe class OverlaySharedState : IDisposable
{
    public const uint AbiVersion = 6;

    /// <summary>Must match <c>overlay::kInputRingCapacity</c>.</summary>
    public const int InputRingCapacity = 512;

    private const int StateSize = 144;
    private const int RingHeaderSize = 16;
    private const int InputEventSize = 20;
    private const int BlockSize = StateSize + RingHeaderSize + InputEventSize * InputRingCapacity;

    private readonly ISharedMemory _shm;
    private readonly SharedState* _state;
    private readonly InputRingHeader* _ring;
    private readonly SharedInputEvent* _ringEvents;

    public uint GamePid { get; }

    private OverlaySharedState(ISharedMemory shm, uint gamePid)
    {
        _shm = shm;
        GamePid = gamePid;

        byte* ptr = shm.Pointer;
        _state = (SharedState*)ptr;
        _ring = (InputRingHeader*)(ptr + StateSize);
        _ringEvents = (SharedInputEvent*)(ptr + StateSize + RingHeaderSize);
    }

    static OverlaySharedState()
    {
        // Mirrors the static_asserts on the C++ side. Getting this wrong means
        // reading another process's memory at the wrong offsets, so check it
        // once at startup rather than discovering it as garbage input.
        Check(sizeof(SharedState), StateSize, nameof(SharedState));
        Check(sizeof(InputRingHeader), RingHeaderSize, nameof(InputRingHeader));
        Check(sizeof(SharedInputEvent), InputEventSize, nameof(SharedInputEvent));

        static void Check(int actual, int expected, string what)
        {
            if (actual != expected)
            {
                throw new InvalidOperationException(
                    $"{what} is {actual} bytes but shared_state.h declares {expected}. " +
                    "The IPC contract has diverged; fix both sides together.");
            }
        }
    }

    /// <summary>
    /// Creates or opens the control block for a game process. Uses
    /// create-or-open semantics so it does not matter whether the host or the
    /// injected payload gets here first.
    /// </summary>
    public static OverlaySharedState CreateOrOpen(uint gamePid)
    {
        var shm = PlatformServices.CreateOrOpenSharedMemory($"AvaloniaOverlay.State.{gamePid}", BlockSize);
        return new OverlaySharedState(shm, gamePid);
    }

    // --- DLL-written fields (read-only from here) -------------------------

    public uint DllAbiVersion => Volatile.Read(ref _state->AbiVersion);
    public bool DllAttached => Volatile.Read(ref _state->DllAttached) != 0;
    public ulong AdapterLuid => _state->AdapterLuid;
    public IntPtr GameHwnd => (IntPtr)_state->GameHwnd;
    public uint GameWidth => Volatile.Read(ref _state->GameWidth);
    public uint GameHeight => Volatile.Read(ref _state->GameHeight);
    public uint SwapchainGeneration => Volatile.Read(ref _state->SwapchainGeneration);
    public uint PresentCount => Volatile.Read(ref _state->PresentCount);
    public uint DrawCount => Volatile.Read(ref _state->DrawCount);
    public uint MutexTimeoutCount => Volatile.Read(ref _state->MutexTimeoutCount);
    public bool BackbufferIsSrgb => _state->BackbufferIsSrgb != 0;
    public GameGraphicsApi GraphicsApi => (GameGraphicsApi)Volatile.Read(ref _state->GraphicsApi);

    // --- host-written fields ----------------------------------------------

    public ulong SharedHandle
    {
        get => _state->SharedHandle;
        set => _state->SharedHandle = value;
    }

    public uint TexWidth { get => _state->TexWidth; set => _state->TexWidth = value; }
    public uint TexHeight { get => _state->TexHeight; set => _state->TexHeight = value; }

    public bool Visible
    {
        get => Volatile.Read(ref _state->Visible) != 0;
        set => Volatile.Write(ref _state->Visible, value ? 1u : 0u);
    }

    /// <summary>
    /// While set, the payload swallows input instead of passing it to the game
    /// and forwards it to us through the ring.
    /// </summary>
    public bool InputCapture
    {
        get => Volatile.Read(ref _state->InputCapture) != 0;
        set => Volatile.Write(ref _state->InputCapture, value ? 1u : 0u);
    }

    public uint InputDropped => Volatile.Read(ref _ring->Dropped);
    public uint InputPushCount => Volatile.Read(ref _state->InputPushCount);
    public uint InputSeenCount => Volatile.Read(ref _state->InputSeenCount);
    public uint RingWriteIndex => Volatile.Read(ref _ring->WriteIndex);
    public uint RingReadIndex => Volatile.Read(ref _ring->ReadIndex);

    /// <summary>
    /// Pops the next input event the payload queued, or false when the ring is
    /// empty.
    /// </summary>
    /// <remarks>
    /// The payload publishes the event body before advancing the write index,
    /// so observing an advanced index guarantees the slot is fully written.
    /// This is the consumer half - only ever called from one thread.
    /// </remarks>
    public bool TryDequeueInput(out SharedInputEvent evt)
    {
        uint read = _ring->ReadIndex;
        if (read == Volatile.Read(ref _ring->WriteIndex))
        {
            evt = default;
            return false;
        }

        evt = _ringEvents[read];
        Volatile.Write(ref _ring->ReadIndex, (read + 1) & (InputRingCapacity - 1));
        return true;
    }

    public uint HostPid { set => _state->HostPid = value; }

    public void AdvanceFrame() => Interlocked.Increment(ref *(int*)&_state->FrameIndex);
    public void Heartbeat() => Interlocked.Increment(ref *(int*)&_state->HostHeartbeat);

    /// <summary>
    /// Publishes a newly created shared texture atomically enough for the DLL:
    /// dimensions are written before the handle, and the handle is what the
    /// payload keys off, so it can never open a texture with stale dimensions.
    /// </summary>
    /// <summary>The payload's D3D12 fence handle, valid in the game process.</summary>
    public ulong ConsumeFenceHandle => Volatile.Read(ref _state->ConsumeFenceHandle);

    /// <summary>Publishes the host's produce fence for a D3D12 payload to open.</summary>
    public void PublishProduceFence(ulong handleInGameProcess)
        => Volatile.Write(ref _state->ProduceFenceHandle, handleInGameProcess);

    /// <summary>The value signalled once the newest frame's copy has landed.</summary>
    public ulong ProduceFenceValue
    {
        get => Volatile.Read(ref _state->ProduceFenceValue);
        set => Volatile.Write(ref _state->ProduceFenceValue, value);
    }

    // --- D3D9 CPU frame transport (host writes) ---------------------------

    /// <summary>Generation of the CPU pixel mapping; bumped on every (re)create.</summary>
    public uint CpuFrameGeneration
    {
        get => Volatile.Read(ref _state->CpuFrameGeneration);
        set => Volatile.Write(ref _state->CpuFrameGeneration, value);
    }

    /// <summary>Seqlock over the CPU pixel buffer: odd while writing, even when readable.</summary>
    public uint CpuFrameSeq
    {
        get => Volatile.Read(ref _state->CpuFrameSeq);
        set => Volatile.Write(ref _state->CpuFrameSeq, value);
    }

    /// <summary>
    /// Logical name of the per-generation CPU pixel mapping (prefix-free; the
    /// shared-memory backend adds any OS namespace prefix). The OS object name
    /// this resolves to mirrors the payload's FormatFrameMappingName.
    /// </summary>
    public string FrameMappingName(uint generation)
        => $"AvaloniaOverlay.Frame.{GamePid}.{generation}";

    public void PublishTexture(ulong handleInGameProcess, uint width, uint height)
    {
        _state->TexWidth = width;
        _state->TexHeight = height;
        Thread.MemoryBarrier();
        Volatile.Write(ref _state->SharedHandle, handleInGameProcess);
    }

    public void Dispose() => _shm.Dispose();
}
