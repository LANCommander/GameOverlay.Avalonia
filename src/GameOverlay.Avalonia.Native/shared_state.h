// shared_state.h - IPC contract between the overlay host process and the
// injected payload. Mirrored byte-for-byte by OverlaySharedState.cs.
//
// Transport is a named file mapping plus a shared D3D11 texture. There is no
// pipe and no RPC: the host writes its fields, the DLL writes its fields, and
// neither ever writes the other's. That makes the whole protocol lock-free.
#pragma once

// This header is the platform-neutral IPC contract: it declares only the shared
// memory layout and the lock-free ring helper, with no dependency on any OS SDK.
// The primitives that create/map the section, and the platform-specific object
// naming, live behind the platform layer (see platform/platform.h) so the same
// contract compiles on Windows, Linux and macOS.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>

namespace overlay {

// Bump on any layout change. Both sides refuse to talk across a mismatch
// rather than silently misinterpreting each other's bytes.
//   1 - display-only overlay
//   2 - added the input ring
//   3 - added graphicsApi + shared-fence transport for D3D12
//   4 - added D3D10 (legacy-shared keyed-mutex texture transport)
//   5 - added D3D9 (CPU shared-memory frame transport)
constexpr uint32_t kAbiVersion = 5;

enum : uint32_t {
    kGraphicsApiUnknown = 0,
    // D3D9 has no DXGI and cannot open any GPU-shared texture the other APIs
    // use, so it takes a CPU transport instead: the host copies the overlay
    // pixels into a second shared-memory mapping and the payload uploads them.
    // See cpuFrameGeneration / cpuFrameSeq and FormatFrameMappingName below.
    kGraphicsApiD3D9 = 9,
    // D3D10 predates NT-handle sharing, so it cannot open the D3D11.1 shared
    // texture the other APIs use. The host gives it a *legacy* (GetSharedHandle)
    // keyed-mutex texture instead; the acquire/release handshake is identical.
    kGraphicsApiD3D10 = 10,
    kGraphicsApiD3D11 = 11,
    kGraphicsApiD3D12 = 12,
    // Vulkan uses the D3D11-style keyed-mutex texture, because
    // VK_KHR_win32_keyed_mutex lets it acquire the very same mutex. Only D3D12,
    // which has no keyed mutex at all, needs the fence pair.
    kGraphicsApiVulkan = 13,
    // OpenGL likewise reuses the keyed-mutex texture, aliased in through
    // WGL_NV_DX_interop2.
    kGraphicsApiOpenGL = 14,
};

// Fields are ordered so every member sits on its natural alignment under the
// default 8-byte packing. Do not reorder without updating the C# mirror.
#pragma pack(push, 8)
struct SharedState {
    // --- written by the DLL, read by the host ---------------------------
    uint32_t abiVersion;            // kAbiVersion
    uint32_t gamePid;
    uint64_t adapterLuid;           // LUID of the adapter the game's device is on
    uint64_t gameHwnd;              // swapchain output window
    uint32_t gameWidth;             // backbuffer dimensions
    uint32_t gameHeight;
    uint32_t swapchainGeneration;   // bumped on every ResizeBuffers / mode change
    uint32_t presentCount;          // bumped every Present; proves the game is alive
    uint32_t dllAttached;           // 1 once hooks are installed
    uint32_t backbufferIsSrgb;      // 1 if the backbuffer format is an _SRGB variant

    // --- written by the host, read by the DLL ---------------------------
    uint64_t sharedHandle;          // NT handle VALUE IN THE GAME PROCESS (duplicated in)
    uint32_t texWidth;              // dimensions of the shared overlay texture
    uint32_t texHeight;
    uint32_t frameIndex;            // bumped after each successful publish
    uint32_t visible;               // 0 = skip compositing entirely
    uint32_t inputCapture;          // reserved for the interactivity milestone
    uint32_t hostPid;
    uint32_t hostHeartbeat;         // host bumps periodically; DLL detects a dead host

    // --- diagnostics, written by the DLL --------------------------------
    // Without these, "the overlay is not visible" is indistinguishable from
    // "the overlay is drawing something invisible", which are very different
    // bugs.
    uint32_t drawCount;             // Draw calls actually issued
    uint32_t mutexTimeoutCount;     // frames where the keyed mutex was busy
    uint32_t inputPushCount;        // events the payload pushed onto the ring
    uint32_t inputSeenCount;        // input messages the WndProc observed at all

    // 0 = not yet known, 11 = Direct3D 11, 12 = Direct3D 12. The host needs
    // this before it creates the shared texture: D3D12 has no keyed mutex, so
    // the two APIs require different synchronisation primitives.
    uint32_t graphicsApi;

    // --- D3D12 synchronisation ------------------------------------------
    // D3D12 has no IDXGIKeyedMutex, so the keyed-mutex handshake is rebuilt
    // from a pair of shared fences. Both handle values are valid in the GAME
    // process; each side duplicates the other's across.
    //
    //   produce (host -> payload):  host signals after its copy lands
    //   consume (payload -> host):  payload signals once it has read the frame
    //
    // Neither side ever waits on the GPU: both poll GetCompletedValue and skip
    // a frame instead, which preserves the "never stall the game" guarantee.
    uint64_t produceFenceHandle;    // host writes   (D3D11 shared fence)
    uint64_t consumeFenceHandle;    // payload writes (D3D12 shared fence)
    uint64_t produceFenceValue;     // host writes   — value signalled for the latest frame

    // --- D3D9 CPU frame transport ---------------------------------------
    // D3D9 opens no shared texture, so the host publishes the overlay pixels
    // through a *second* shared-memory mapping named per generation (see
    // FormatFrameMappingName). These two fields, written by the host, live in
    // the main mapping and coordinate access to that pixel buffer:
    //
    //   cpuFrameGeneration  bumped each time the host (re)creates the pixel
    //                       mapping at a new size; the payload keys the mapping
    //                       name off it and reopens when it changes.
    //   cpuFrameSeq         a seqlock: odd while the host is mid-copy, even when
    //                       a whole frame is readable. The payload reads it
    //                       before and after its copy and discards a torn frame
    //                       rather than blocking, preserving the "never stall
    //                       the game" guarantee.
    uint32_t cpuFrameGeneration;    // host writes
    uint32_t cpuFrameSeq;           // host writes
    uint32_t reserved[2];
};
#pragma pack(pop)

static_assert(sizeof(SharedState) == 144, "SharedState layout changed; update OverlaySharedState.cs");
static_assert(offsetof(SharedState, produceFenceHandle) % 8 == 0, "fence handles must be 8-byte aligned");
static_assert(offsetof(SharedState, sharedHandle) % 8 == 0, "sharedHandle must be 8-byte aligned");
static_assert(offsetof(SharedState, adapterLuid) % 8 == 0, "adapterLuid must be 8-byte aligned");

// --------------------------------------------------------------------------
// Input transport
// --------------------------------------------------------------------------

// What the payload saw. Deliberately a flat POD rather than a tagged union so
// the ring is a plain array with no pointers to translate across processes.
enum class InputEventType : uint32_t {
    None = 0,
    MouseMove,          // x,y = client pixels
    MouseMoveDelta,     // x,y = raw relative delta (game has the cursor captured)
    MouseDown,          // data = MouseButton
    MouseUp,            // data = MouseButton
    MouseWheel,         // data = signed wheel delta (WHEEL_DELTA units), cast to int32
    MouseHWheel,
    KeyDown,            // data = virtual key, y = lParam key data (scan code, extended bit)
    KeyUp,
    Char,               // data = UTF-16 code unit
};

enum class MouseButton : uint32_t {
    Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4,
};

// Modifier bitmask. Ours rather than Avalonia's or Win32's, so neither side
// depends on the other's enum values.
enum : uint32_t {
    kModShift  = 1u << 0,
    kModControl= 1u << 1,
    kModAlt    = 1u << 2,
    kModLeft   = 1u << 3,   // mouse buttons currently held
    kModRight  = 1u << 4,
    kModMiddle = 1u << 5,
};

struct InputEvent {
    uint32_t type;          // InputEventType
    int32_t  x;
    int32_t  y;
    uint32_t data;
    uint32_t modifiers;
};

// Power of two so the index wrap is a mask.
constexpr uint32_t kInputRingCapacity = 512;
static_assert((kInputRingCapacity & (kInputRingCapacity - 1)) == 0, "capacity must be a power of two");

// Single producer (the game's message thread) / single consumer (the host's
// drain thread). Neither side ever blocks: if the ring is full the payload
// drops the newest event and counts it, because stalling a game's message
// pump to wait on another process is never acceptable.
//
// The indices are plain uint32_t rather than volatile: the payload accesses
// them through std::atomic_ref (see PushInputEvent) for portable, correctly
// ordered publication, and the host reads them from the other process with its
// own Volatile.Read. atomic_ref requires a non-volatile object, so the ordering
// lives in the accesses, not the field qualifier.
struct InputRing {
    uint32_t writeIndex;   // payload writes
    uint32_t readIndex;    // host writes
    uint32_t dropped;      // payload writes; diagnostics
    uint32_t _pad;
    InputEvent events[kInputRingCapacity];
};

// One mapping holds both. Keeping them together means a single handshake and
// a single name to agree on.
struct SharedBlock {
    SharedState state;
    InputRing   input;
};

static_assert(sizeof(InputEvent) == 20, "InputEvent layout changed; update OverlaySharedState.cs");
static_assert(offsetof(SharedBlock, input) == 144, "SharedBlock layout changed; update OverlaySharedState.cs");
static_assert(sizeof(SharedBlock) == 144 + 16 + 20 * kInputRingCapacity,
              "SharedBlock layout changed; update OverlaySharedState.cs");

// Both sides derive the mapping name from the game pid, so multiple overlaid
// games can coexist without colliding.
inline void FormatMappingName(wchar_t (&buffer)[64], uint32_t gamePid) {
    std::swprintf(buffer, std::size(buffer), L"Local\\AvaloniaOverlay.State.%u", gamePid);
}

// Name of the D3D9 CPU pixel-buffer mapping. The generation is part of the name
// so a resize creates a genuinely new section rather than colliding with the
// old one (a named mapping ignores the size passed to CreateFileMapping if one
// with that name already exists), and both sides release the old one naturally
// once neither holds a handle.
inline void FormatFrameMappingName(wchar_t (&buffer)[80], uint32_t gamePid, uint32_t generation) {
    std::swprintf(buffer, std::size(buffer), L"Local\\AvaloniaOverlay.Frame.%u.%u", gamePid, generation);
}

// The mapped view, or nullptr before the payload has finished initializing.
// Defined in dllmain.cpp.
SharedState* GetSharedState();
InputRing*   GetInputRing();

// Appends an event to the ring from the game's message thread.
//
// Publishing the payload before the index (with a release fence between) is
// what lets the host read without a lock: it can never observe an advanced
// index pointing at an unwritten slot.
inline void PushInputEvent(InputRing* ring, const InputEvent& event) {
    if (!ring) return;

    std::atomic_ref<uint32_t> writeRef(ring->writeIndex);
    std::atomic_ref<uint32_t> readRef(ring->readIndex);

    const uint32_t write = writeRef.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) & (kInputRingCapacity - 1);
    if (next == readRef.load(std::memory_order_acquire)) {
        std::atomic_ref<uint32_t>(ring->dropped).fetch_add(1, std::memory_order_relaxed);
        return;   // full: drop the newest rather than stall the game
    }

    ring->events[write] = event;
    // Release store: publishes the fully written event body before the index
    // advance becomes visible, so the consumer can never read an unwritten slot.
    writeRef.store(next, std::memory_order_release);
}

} // namespace overlay
