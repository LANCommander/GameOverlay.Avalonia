#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <cstdint>

#include "shared_state.h"

namespace overlay {

// Composites the host's shared texture onto the game's backbuffer.
//
// Every method here runs on the game's render thread inside the Present hook,
// so the hard rules are: no heap allocation, no blocking wait, no Flush, and
// no unbounded work. Anything that would violate those belongs in the host.
class D3D11Renderer {
public:
    // Lazily binds to whatever device the game's swapchain belongs to.
    // Safe to call every frame; returns false until it can do useful work.
    bool EnsureInitialized(IDXGISwapChain* swapChain, SharedState* state);

    // Draws the overlay. Assumes EnsureInitialized returned true.
    void Render(IDXGISwapChain* swapChain, SharedState* state);

    // Called from the ResizeBuffers hook: the backbuffer and therefore our
    // cached RTV are about to become invalid.
    void OnResizeBuffers();

    void Shutdown();

    bool initialized() const { return initialized_; }

private:
    bool CreatePipeline();
    bool OpenSharedTexture(SharedState* state);
    bool EnsureRenderTarget(IDXGISwapChain* swapChain);
    void ReleaseSharedTexture();

    ID3D11Device*           device_ = nullptr;
    ID3D11Device1*          device1_ = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;

    ID3D11VertexShader*     vs_ = nullptr;
    ID3D11PixelShader*      ps_ = nullptr;
    ID3D11SamplerState*     sampler_ = nullptr;
    ID3D11BlendState*       blend_ = nullptr;
    ID3D11RasterizerState*  raster_ = nullptr;
    ID3D11DepthStencilState* depth_ = nullptr;

    // The texture the host writes into, and our private copy of it. The copy
    // is what lets us both never block the game and never flicker: if the
    // keyed mutex is busy this frame we simply redraw the previous contents.
    ID3D11Texture2D*          sharedTex_ = nullptr;
    IDXGIKeyedMutex*          sharedMutex_ = nullptr;
    ID3D11Texture2D*          privateTex_ = nullptr;
    ID3D11ShaderResourceView* privateSrv_ = nullptr;
    bool                      privateTexHasContent_ = false;

    // Cached backbuffer RTV, invalidated by generation rather than rebuilt
    // per frame.
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D*        rtvBackbuffer_ = nullptr;

    uint64_t openedHandle_ = 0;
    uint32_t openedTexWidth_ = 0;
    uint32_t openedTexHeight_ = 0;

    bool initialized_ = false;
    bool pipelineFailed_ = false;
};

} // namespace overlay
