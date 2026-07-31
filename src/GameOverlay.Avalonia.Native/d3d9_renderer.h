#pragma once

#include <d3d9.h>
#include <cstdint>
#include <vector>

#include "shared_state.h"

namespace overlay {

// Composites the host's overlay onto a Direct3D 9 game's backbuffer.
//
// D3D9 has none of the GPU-sharing machinery the other backends rely on - no
// DXGI, no keyed mutex, no shared NT handle - so this renderer takes the CPU
// transport instead: it opens the host's shared-memory pixel buffer, copies a
// tear-free frame out of it under the seqlock, uploads that into a dynamic
// texture, and draws a fullscreen quad with the fixed-function pipeline.
//
// Everything runs on the game's render thread inside the EndScene hook. The
// per-frame cost is two CPU copies (mapping -> staging -> texture) plus the
// draw; there is no blocking wait and no allocation once the size is stable.
class D3D9Renderer {
public:
    // Draws the overlay onto the current backbuffer. Assumes the caller is
    // inside a BeginScene/EndScene pair (the EndScene hook is).
    void Render(IDirect3DDevice9* device, SharedState* state);

    // Called from the Reset hook before the device is reset: every D3DPOOL_DEFAULT
    // resource must be released or Reset fails.
    void OnDeviceLost();

    void Shutdown();

private:
    bool EnsureTexture(IDirect3DDevice9* device, uint32_t width, uint32_t height);
    bool OpenFrameMapping(SharedState* state);
    void ReleaseFrameMapping();
    bool UploadFrame(SharedState* state);
    void DrawQuad(IDirect3DDevice9* device);

    IDirect3DTexture9* tex_ = nullptr;      // D3DPOOL_DEFAULT, dynamic, A8R8G8B8
    uint32_t           texWidth_ = 0;
    uint32_t           texHeight_ = 0;
    bool               texHasContent_ = false;

    // The host's pixel mapping, reopened whenever its generation changes.
    HANDLE         frameMap_ = nullptr;
    const uint8_t* framePixels_ = nullptr;
    uint32_t       openedGeneration_ = 0;

    // Validated frame, copied out of the mapping under the seqlock before it is
    // uploaded. Sized on resize, never in the hot path.
    std::vector<uint8_t> staging_;
};

} // namespace overlay
