using System;
using System.Runtime.InteropServices;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace GameOverlay.Avalonia;

/// <summary>
/// Owns the host-side D3D11 device and the shared texture the injected payload
/// composites from.
///
/// The device is deliberately created on the *game's* adapter: a shared texture
/// cannot cross adapters, and on a hybrid laptop the default adapter is very
/// often not the one the game is rendering on.
/// </summary>
internal sealed unsafe class SharedTextureProducer : IFrameProducer
{
    private const int S_OK = 0;
    private const int WAIT_TIMEOUT = 0x00000102;
    private const int WAIT_ABANDONED = 0x00000080;

    // Keyed-mutex protocol. The host acquires at 0 and releases to 1; the
    // payload acquires at 1 and releases back to 0. The texture is created
    // already released at key 0, so the host always produces the first frame.
    private const ulong KeyHostAcquire = 0;
    private const ulong KeyPayloadAcquire = 1;

    private readonly IProcessInjector _game;
    private readonly OverlaySharedState _state;

    private ID3D11Device _device = null!;
    private ID3D11DeviceContext _context = null!;

    private ID3D11Texture2D? _shared;
    private ID3D11Texture2D? _upload;
    private IDXGIKeyedMutex? _mutex;
    private IntPtr _mutexPtr;
    private IntPtr _localHandle;
    private IntPtr _remoteHandle;

    // --- D3D12 path -------------------------------------------------------
    // D3D12 has no keyed mutex, so the same "producer and consumer never touch
    // the texture at once" guarantee is rebuilt from two shared fences.
    private readonly bool _useFenceSync;

    // --- D3D10 path -------------------------------------------------------
    // D3D10 predates NT-handle sharing and cannot open the D3D11.1 texture the
    // other keyed-mutex backends use. It gets a *legacy* (GetSharedHandle)
    // keyed-mutex texture instead: same keyed-mutex handshake, but the handle
    // is a machine-global value that needs no per-process duplication.
    private readonly bool _useLegacyShare;
    private ID3D11Device5? _device5;
    private ID3D11DeviceContext4? _context4;
    private ID3D11Fence? _produceFence;
    private IntPtr _produceFenceLocalHandle;
    private IntPtr _produceFenceRemoteHandle;
    private ID3D11Fence? _consumeFence;
    private ulong _producedValue;

    private readonly Action<string>? _log;

    public int Width { get; private set; }
    public int Height { get; private set; }

    public SharedTextureProducer(IProcessInjector game, OverlaySharedState state, Action<string>? log = null)
    {
        _game = game;
        _state = state;
        _log = log;
        _useFenceSync = state.GraphicsApi == GameGraphicsApi.D3D12;
        _useLegacyShare = state.GraphicsApi == GameGraphicsApi.D3D10;
        CreateDeviceOnGameAdapter();

        if (_useFenceSync) CreateProduceFence();
    }

    private void Log(string message) => _log?.Invoke(message);

    /// <summary>
    /// Creates the host-to-payload fence and publishes it for a D3D12 payload.
    /// </summary>
    /// <remarks>
    /// A D3D11 fence and a D3D12 fence are the same underlying object once
    /// shared, which is what makes the cross-API handshake possible at all -
    /// unlike <c>IDXGIKeyedMutex</c>, which D3D12 does not implement.
    /// </remarks>
    private void CreateProduceFence()
    {
        _device5 = _device.QueryInterfaceOrNull<ID3D11Device5>()
            ?? throw new InvalidOperationException(
                "ID3D11Device5 unavailable; cross-API fence sharing requires D3D11.4.");
        _context4 = _context.QueryInterfaceOrNull<ID3D11DeviceContext4>()
            ?? throw new InvalidOperationException("ID3D11DeviceContext4 unavailable.");

        _produceFence = _device5.CreateFence(0, FenceFlags.Shared);
        // Name must be null, not empty: an empty string is not "unnamed", it is
        // an invalid name, and the runtime rejects it.
        // null! because Vortice types the name as non-nullable, but a null name
        // is exactly what "unnamed handle" means to the runtime.
        _produceFenceLocalHandle = _produceFence.CreateSharedHandle(null, null!);
        _produceFenceRemoteHandle = _game.DuplicateHandleInto(_produceFenceLocalHandle);
        _state.PublishProduceFence((ulong)_produceFenceRemoteHandle);

        Log($"[producer] D3D12 game: fence sync, produce fence remote handle " +
                          $"0x{_produceFenceRemoteHandle:X}");
    }

    /// <summary>
    /// Opens the payload's fence once it has published one. Until then the
    /// producer runs unthrottled, which is safe because the payload cannot be
    /// reading a frame it has not been told about yet.
    /// </summary>
    private void TryOpenConsumeFence()
    {
        if (_consumeFence is not null || _device5 is null) return;

        ulong remote = _state.ConsumeFenceHandle;
        if (remote == 0) return;

        try
        {
            IntPtr local = _game.DuplicateHandleFrom((IntPtr)remote);
            _consumeFence = _device5.OpenSharedFence<ID3D11Fence>(local);
            Log("[producer] opened payload consume fence");
        }
        catch (Exception ex)
        {
            Log($"[producer] could not open consume fence: {ex.Message}");
        }
    }

    private void CreateDeviceOnGameAdapter()
    {
        ulong wantedLuid = _state.AdapterLuid;

        using var factory = DXGI.CreateDXGIFactory1<IDXGIFactory1>();

        IDXGIAdapter1? chosen = null;
        string chosenName = "<default>";

        for (uint i = 0; factory.EnumAdapters1(i, out IDXGIAdapter1 adapter).Success; i++)
        {
            var desc = adapter.Description1;
            ulong luid = unchecked((ulong)(((long)desc.Luid.HighPart << 32) | (uint)desc.Luid.LowPart));
            if (wantedLuid != 0 && luid == wantedLuid)
            {
                chosen = adapter;
                chosenName = desc.Description;
                break;
            }
            adapter.Dispose();
        }

        if (wantedLuid != 0 && chosen is null)
        {
            // Failing loudly here is deliberate. The silent alternative is a
            // texture the game simply cannot open, which surfaces much later
            // as a blank overlay with no obvious cause.
            throw new InvalidOperationException(
                $"No DXGI adapter matches the game's LUID 0x{wantedLuid:X16}. " +
                "Cross-adapter texture sharing is not supported; run the game and the " +
                "host on the same GPU.");
        }

        var levels = new[] { FeatureLevel.Level_11_1, FeatureLevel.Level_11_0 };
        var result = D3D11.D3D11CreateDevice(
            chosen,
            chosen is null ? DriverType.Hardware : DriverType.Unknown,
            DeviceCreationFlags.BgraSupport,
            levels,
            out ID3D11Device device,
            out ID3D11DeviceContext context);

        chosen?.Dispose();
        result.CheckError();

        _device = device;
        _context = context;
        Log($"[producer] device created on adapter {chosenName} (LUID 0x{wantedLuid:X16})");
    }

    /// <summary>
    /// (Re)creates the shared texture at the requested size and republishes it
    /// to the payload. Cheap no-op when the size is unchanged.
    /// </summary>
    public void EnsureSize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        if (_shared is not null && width == Width && height == Height) return;

        ReleaseTextures();

        Width = width;
        Height = height;

        // BGRA8 premultiplied: matches Avalonia/Skia's Bgra8888 output exactly,
        // so the upload is a straight memcpy with no swizzle or conversion.
        //
        // The keyed mutex is deliberately omitted for D3D12: ID3D12Device
        // refuses to open a shared resource that carries one, and there is no
        // D3D12 equivalent to acquire it with. Those games get fence sync.
        //
        // SharedNTHandle is not valid on its own: it must be paired with either
        // Shared or SharedKeyedMutex, so the D3D12 path takes plain Shared.
        //
        // D3D10 is the exception: it cannot open an NT-handle resource at all,
        // so it gets a *legacy* keyed-mutex texture (SharedKeyedMutex without
        // SharedNTHandle), shared through the old GetSharedHandle mechanism.
        var options = _useLegacyShare
            ? ResourceOptionFlags.SharedKeyedMutex
            : ResourceOptionFlags.SharedNTHandle
              | (_useFenceSync ? ResourceOptionFlags.Shared
                               : ResourceOptionFlags.SharedKeyedMutex);

        var sharedDesc = new Texture2DDescription(
            Format.B8G8R8A8_UNorm, (uint)width, (uint)height,
            arraySize: 1, mipLevels: 1,
            BindFlags.ShaderResource,
            ResourceUsage.Default,
            CpuAccessFlags.None,
            sampleCount: 1, sampleQuality: 0,
            options);

        _shared = _device.CreateTexture2D(sharedDesc);

        // Dynamic + WriteDiscard rather than a staging texture: WriteDiscard
        // hands back a fresh buffer every frame and therefore never stalls
        // waiting for the previous CopyResource to drain. We rewrite every
        // pixel from Avalonia's persistent buffer anyway, so discarding the
        // old contents costs us nothing.
        var uploadDesc = new Texture2DDescription(
            Format.B8G8R8A8_UNorm, (uint)width, (uint)height,
            arraySize: 1, mipLevels: 1,
            BindFlags.ShaderResource,
            ResourceUsage.Dynamic,
            CpuAccessFlags.Write,
            sampleCount: 1, sampleQuality: 0,
            ResourceOptionFlags.None);

        _upload = _device.CreateTexture2D(uploadDesc);

        if (_useLegacyShare)
        {
            // Legacy shared handle: a machine-global value the payload can pass
            // straight to ID3D10Device::OpenSharedResource in its own process,
            // so there is nothing to duplicate. It is owned by the resource and
            // must not be closed like a real handle - hence _localHandle and
            // _remoteHandle stay zero and ReleaseTextures leaves it alone.
            using var resource = _shared.QueryInterface<IDXGIResource>();
            IntPtr legacyHandle = resource.SharedHandle;

            _mutex = _shared.QueryInterface<IDXGIKeyedMutex>();
            _mutexPtr = _mutex.NativePointer;

            _state.PublishTexture((ulong)legacyHandle, (uint)width, (uint)height);
            Log($"[producer] legacy shared texture {width}x{height}, handle 0x{legacyHandle:X}");
            return;
        }

        using (var resource = _shared.QueryInterface<IDXGIResource1>())
        {
            _localHandle = resource.CreateSharedHandle(
                null,
                Vortice.DXGI.SharedResourceFlags.Read | Vortice.DXGI.SharedResourceFlags.Write,
                null);
        }

        if (!_useFenceSync)
        {
            _mutex = _shared.QueryInterface<IDXGIKeyedMutex>();
            _mutexPtr = _mutex.NativePointer;
        }

        // The payload cannot use our handle value directly - handles are
        // per-process - so duplicate it into the game and publish that.
        _remoteHandle = _game.DuplicateHandleInto(_localHandle);
        _state.PublishTexture((ulong)_remoteHandle, (uint)width, (uint)height);

        Log($"[producer] shared texture {width}x{height}, " +
                          $"local handle 0x{_localHandle:X}, remote handle 0x{_remoteHandle:X}");
    }

    /// <summary>
    /// Uploads one frame of premultiplied BGRA pixels and hands it to the
    /// payload. Returns false when the payload has not yet consumed the
    /// previous frame, in which case the caller should simply try again later.
    /// </summary>
    public bool TryPublishFrame(IntPtr source, int sourceRowBytes)
    {
        if (_shared is null || _upload is null) return false;

        if (_useFenceSync)
        {
            TryOpenConsumeFence();

            // The payload signals the produced value back once its GPU work has
            // read the frame. Until then the texture is still in use, so skip
            // rather than overwrite it - the same contract the keyed mutex
            // enforces on the D3D11 path, just expressed with fences.
            if (_consumeFence is not null && _producedValue > 0 &&
                _consumeFence.CompletedValue < _producedValue)
            {
                return false;
            }

            UploadAndCopy(source, sourceRowBytes);

            // Signal only after the copy is queued, so the payload cannot
            // observe the new value before the pixels have landed.
            _producedValue++;
            _context4!.Signal(_produceFence!, _producedValue);
            _context.Flush();
            _state.ProduceFenceValue = _producedValue;

            _state.AdvanceFrame();
            return true;
        }

        if (_mutexPtr == IntPtr.Zero) return false;

        // Zero timeout throughout: the producer must never block, so a busy
        // mutex just means "the payload is mid-copy, skip this frame".
        int hr = AcquireSync(_mutexPtr, KeyHostAcquire, 0);
        if (hr == WAIT_TIMEOUT) return false;

        // WAIT_ABANDONED means the payload's process died holding the mutex.
        // Ownership has passed to us, so we must go on to release it; treating
        // it as an error would strand the mutex.
        if (hr != S_OK && hr != WAIT_ABANDONED)
        {
            throw new InvalidOperationException($"AcquireSync failed: 0x{hr:X8}");
        }

        try
        {
            UploadAndCopy(source, sourceRowBytes);

            // Flush before releasing the mutex so the copy is actually
            // submitted rather than sitting in our command buffer while the
            // payload starts reading. This costs the host a submit; it costs
            // the game nothing, which is the whole point of the split.
            _context.Flush();
        }
        finally
        {
            ReleaseSync(_mutexPtr, KeyPayloadAcquire);
        }

        _state.AdvanceFrame();
        return true;
    }

    /// <summary>
    /// Copies the CPU frame into the upload texture and on into the shared one.
    /// Identical for both sync modes; only the surrounding handshake differs.
    /// </summary>
    private void UploadAndCopy(IntPtr source, int sourceRowBytes)
    {
        MappedSubresource mapped = _context.Map(_upload!, 0, MapMode.WriteDiscard,
                                                Vortice.Direct3D11.MapFlags.None);
        try
        {
            // Defence in depth: the session already refuses to publish a
            // mismatched source, but clamping here means a future caller
            // cannot turn a sizing mistake into an out-of-bounds read.
            int rowBytes = Math.Min(sourceRowBytes, (int)mapped.RowPitch);
            byte* dst = (byte*)mapped.DataPointer;
            byte* src = (byte*)source;
            for (int y = 0; y < Height; y++)
            {
                Buffer.MemoryCopy(src + (long)y * sourceRowBytes,
                                  dst + (long)y * mapped.RowPitch,
                                  mapped.RowPitch, (uint)rowBytes);
            }
        }
        finally
        {
            _context.Unmap(_upload!, 0);
        }

        _context.CopyResource(_shared!, _upload!);
    }

    // Vortice's generated AcquireSync returns void and only throws on a
    // negative HRESULT. WAIT_TIMEOUT (0x102) is non-negative, so a contended
    // mutex would be silently indistinguishable from a successful acquire -
    // and we would publish over a frame the payload is still reading. Calling
    // through the vtable is the only way to see the real result.
    //
    // IDXGIKeyedMutex vtable: IUnknown 0-2, IDXGIObject 3-6,
    // IDXGIDeviceSubObject 7 (GetDevice), then AcquireSync 8, ReleaseSync 9.
    private static int AcquireSync(IntPtr mutex, ulong key, uint milliseconds)
    {
        void** vtbl = *(void***)mutex;
        return ((delegate* unmanaged[Stdcall]<IntPtr, ulong, uint, int>)vtbl[8])(mutex, key, milliseconds);
    }

    private static int ReleaseSync(IntPtr mutex, ulong key)
    {
        void** vtbl = *(void***)mutex;
        return ((delegate* unmanaged[Stdcall]<IntPtr, ulong, int>)vtbl[9])(mutex, key);
    }

    private void ReleaseTextures()
    {
        _mutex?.Dispose();
        _mutex = null;
        _mutexPtr = IntPtr.Zero;

        _upload?.Dispose();
        _upload = null;

        _shared?.Dispose();
        _shared = null;

        if (_localHandle != IntPtr.Zero)
        {
            CloseHandle(_localHandle);
            _localHandle = IntPtr.Zero;
        }

        if (_remoteHandle != IntPtr.Zero)
        {
            // The payload keeps its own D3D reference to the texture, so
            // closing the handle we duplicated in does not pull it out from
            // under a frame in flight.
            _state.SharedHandle = 0;
            try { _game.CloseRemoteHandle(_remoteHandle); }
            catch { /* the game may already be gone */ }
            _remoteHandle = IntPtr.Zero;
        }
    }

    public void Dispose()
    {
        ReleaseTextures();

        _consumeFence?.Dispose();
        _produceFence?.Dispose();
        if (_produceFenceLocalHandle != IntPtr.Zero) CloseHandle(_produceFenceLocalHandle);
        if (_produceFenceRemoteHandle != IntPtr.Zero)
        {
            try { _game.CloseRemoteHandle(_produceFenceRemoteHandle); }
            catch { /* the game may already be gone */ }
        }

        _context4?.Dispose();
        _device5?.Dispose();
        _context?.Dispose();
        _device?.Dispose();
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
