#pragma once

#include "third_party/d3d8/d3d8.h"
#include <cstdint>
#include <vector>

#include "shared_state.h"

namespace overlay {

// Composites the host's overlay onto a Direct3D 8 game's backbuffer.
//
// D3D8, like D3D9, has none of the GPU-sharing machinery the DXGI backends rely
// on - no DXGI, no keyed mutex, no shared NT handle - so it takes the very same
// CPU transport the D3D9 renderer does: it opens the host's shared-memory pixel
// buffer, copies a tear-free frame out of it under the seqlock, uploads that
// into a dynamic texture, and draws a fullscreen quad with the fixed-function
// pipeline.
//
// Everything runs on the game's render thread inside the EndScene hook. The
// per-frame cost is two CPU copies (mapping -> staging -> texture) plus the
// draw; there is no blocking wait and no allocation once the size is stable.
//
// The only differences from D3D9 are the ones the API forces: FVF is selected
// through SetVertexShader rather than SetFVF, state is saved through a DWORD
// state-block token rather than an interface, and the sampler filter/address
// state lives on the texture stage rather than a separate sampler.
class D3D8Renderer {
public:
    // Draws the overlay onto the current backbuffer. Assumes the caller is
    // inside a BeginScene/EndScene pair (the EndScene hook is).
    void Render(IDirect3DDevice8* device, SharedState* state);

    // Called from the Reset hook before the device is reset: every D3DPOOL_DEFAULT
    // resource must be released or Reset fails.
    void OnDeviceLost();

    void Shutdown();

private:
    bool EnsureTexture(IDirect3DDevice8* device, uint32_t width, uint32_t height);
    bool OpenFrameMapping(SharedState* state);
    void ReleaseFrameMapping();
    bool UploadFrame(SharedState* state);
    void DrawQuad(IDirect3DDevice8* device);

    IDirect3DTexture8* tex_ = nullptr;      // D3DPOOL_DEFAULT, dynamic, A8R8G8B8
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
