#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <cstdint>

#include "shared_state.h"

namespace overlay {

// Composites the overlay onto an OpenGL game's default framebuffer.
//
// OpenGL has no swapchain and no native cross-API sharing, so this is the only
// backend that bridges through a second API: it opens the host's D3D11 texture
// (exactly as the D3D11 path does, keyed mutex and all) and aliases it into GL
// with WGL_NV_DX_interop2, then draws a textured triangle over framebuffer 0.
//
// Everything runs on the game's render thread inside the wglSwapBuffers hook,
// where the game's GL context is current. The game's GL state is saved and
// restored around the draw, the same discipline the D3D11 path applies to the
// device context.
class OpenGLRenderer {
public:
    // Draws the overlay for the current context. Safe to call every swap;
    // does nothing until it has a shared texture to show.
    void Render(HDC hdc, SharedState* state);

    void Shutdown();

private:
    bool EnsureFunctions();
    bool EnsureD3DDevice(SharedState* state);
    bool EnsurePipeline();
    bool EnsureSharedTexture(SharedState* state);
    void ReleaseSharedTexture();

    bool functionsLoaded_ = false;
    bool pipelineReady_ = false;
    bool failed_ = false;

    // The interop device handle and the D3D side of the shared texture.
    void*                     interopDevice_ = nullptr;   // HANDLE from wglDXOpenDeviceNV
    ID3D11Device*             d3dDevice_ = nullptr;
    ID3D11DeviceContext*      d3dContext_ = nullptr;
    ID3D11Texture2D*          sharedTex_ = nullptr;
    IDXGIKeyedMutex*          sharedMutex_ = nullptr;

    // WGL_NV_DX_interop cannot register a keyed-mutex texture, so the host's
    // texture is copied into this plain private one, which is what GL aliases.
    // The alias is kept GL-locked persistently and only unlocked for the copy
    // on a new frame - locking every swap is what made this backend costly.
    ID3D11Texture2D*          privateTex_ = nullptr;
    unsigned int              glTexture_ = 0;             // GLuint
    void*                     interopObject_ = nullptr;   // HANDLE from wglDXRegisterObjectNV
    bool                      locked_ = false;
    bool                      hasContent_ = false;
    uint32_t                  lastFrameIndex_ = 0;

    // GL objects for the composite pass.
    unsigned int              program_ = 0;
    unsigned int              vao_ = 0;
    unsigned int              sampler_ = 0;

    uint64_t                  openedHandle_ = 0;
    uint32_t                  texWidth_ = 0;
    uint32_t                  texHeight_ = 0;
};

} // namespace overlay
