#pragma once

#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <cstdint>

#include "shared_state.h"

namespace overlay {

// Composites the host's shared texture onto a Direct3D 10 game's backbuffer.
//
// Structurally identical to D3D11Renderer - same "copy under the keyed mutex
// into a private texture, then draw a fullscreen triangle, restoring all game
// state" shape - but D3D10 differs in two ways that matter here:
//
//   * The device *is* the immediate context: there is no ID3D10DeviceContext.
//   * D3D10 predates NT-handle sharing, so it cannot open the D3D11.1 texture
//     the other backends use. The host publishes a *legacy* (GetSharedHandle)
//     keyed-mutex texture for D3D10 games, which ID3D10Device::OpenSharedResource
//     can open. Acquiring an IDXGIKeyedMutex on that resource requires a D3D10.1
//     device; a pure 10.0 game gets no overlay (logged once), never a crash.
//
// Every method runs on the game's render thread inside the Present hook, so the
// same hard rules apply: no heap allocation, no blocking wait, no Flush.
class D3D10Renderer {
public:
    bool EnsureInitialized(IDXGISwapChain* swapChain, SharedState* state);
    void Render(IDXGISwapChain* swapChain, SharedState* state);
    void OnResizeBuffers();
    void Shutdown();

    bool initialized() const { return initialized_; }

private:
    bool CreatePipeline();
    bool OpenSharedTexture(SharedState* state);
    bool EnsureRenderTarget(IDXGISwapChain* swapChain);
    void ReleaseSharedTexture();

    // ID3D10Device1 rather than ID3D10Device: OpenSharedResource lives on the
    // base interface, but a keyed mutex on the opened resource is only available
    // through the 10.1 runtime, so binding to .1 up front is what proves the
    // shared texture will be usable at all.
    ID3D10Device1* device_ = nullptr;

    ID3D10VertexShader*      vs_ = nullptr;
    ID3D10PixelShader*       ps_ = nullptr;
    ID3D10SamplerState*      sampler_ = nullptr;
    ID3D10BlendState*        blend_ = nullptr;
    ID3D10RasterizerState*   raster_ = nullptr;
    ID3D10DepthStencilState* depth_ = nullptr;

    // The host's texture and our private copy of it. Copying under the mutex is
    // what lets a busy mutex degrade to "redraw the last frame" rather than
    // block the game or flicker.
    ID3D10Texture2D*          sharedTex_ = nullptr;
    IDXGIKeyedMutex*          sharedMutex_ = nullptr;
    ID3D10Texture2D*          privateTex_ = nullptr;
    ID3D10ShaderResourceView* privateSrv_ = nullptr;
    bool                      privateTexHasContent_ = false;

    ID3D10RenderTargetView* rtv_ = nullptr;
    ID3D10Texture2D*        rtvBackbuffer_ = nullptr;

    uint64_t openedHandle_ = 0;
    uint32_t openedTexWidth_ = 0;
    uint32_t openedTexHeight_ = 0;

    bool initialized_ = false;
    bool pipelineFailed_ = false;
};

} // namespace overlay
